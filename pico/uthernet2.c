/**
 * Uthernet II (W5100) emulation at C0x4–C0x7 (slot 4: $C0C4–$C0C7).
 * W5100 register and memory state; C0x interface; network stack and RX path.
 */
#include "uthernet2.h"
#include "uthernet2_net.h"
#include "u2_monitor.h"
#include "w5100_regs.h"
#include "ipc.h"
#include "a2bus.h"
#include "hardware/pio.h"
#include "pico.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"
#include <stdio.h>
#include <string.h>
#if U2_RX_AUDIT
#include "pico/stdio.h"
#include "pico/stdio_uart.h"
#endif

#define READFLAG  (1u << 4)

/* RP2350: U2 runs on core1 inside BusLoop (RAM). These helpers are on every $C0C4–$C0C7 cycle;
 * keep them in SRAM to avoid XIP wait states between IRQ0 clear and UpdateMegaFlashRegisters. */
#ifndef PICO_RP2040
#define U2_BUS_RAM(fn) __time_critical_func(fn)
#else
#define U2_BUS_RAM(fn) fn
#endif

#ifndef U2_MON_LOG_BUS
#define U2_MON_LOG_BUS 0
#endif
#ifndef U2_IP65_TRACE_DATA
#define U2_IP65_TRACE_DATA 0
#endif
#ifndef U2_IP65_CHECKPOINT
#define U2_IP65_CHECKPOINT 0
#endif
#ifndef U2_MACRAW_COMPAT_DROP_OLDEST
#define U2_MACRAW_COMPAT_DROP_OLDEST 0
#endif

/* §1dh RX delivery audit (measurement only, no behaviour change).
 *
 * Settles H1 (dropped bus cycle) with arithmetic instead of guesswork. The host tells us how many
 * bytes it believes it consumed — it advances Sn_RX_RD by exactly that much before issuing RECV —
 * and read_value() sees every real $C0C7 DATA read. If the a2buslistener FIFO silently discards a
 * cycle (push noblock), u2_data_address never advances, the 6502 latches the previous byte again,
 * and the rest of the frame shifts by one: a bad checksum. In that case our observed read count
 * comes out BELOW the host's Sn_RX_RD advance, and the deficit is exactly the number of lost
 * cycles. Frames the driver discards without reading show up as seen==0 (counted separately as
 * "skip") so they cannot be mistaken for drops. */
#ifndef U2_RX_AUDIT
#define U2_RX_AUDIT 0
#endif

#if UTHERNET2_DEBUG && U2_IP65_TRACE_DATA
/* Verbose: after MR=0x03, log next N DATA reads (enable with -DU2_IP65_TRACE_DATA=1). */
static int u2_ip65_data_trace_left;

static void u2_arm_ip65_data_trace(uint8_t mode_byte) {
  if (mode_byte == 0x03)
    u2_ip65_data_trace_left = 48;
}
#else
static void u2_arm_ip65_data_trace(uint8_t mode_byte) { (void)mode_byte; }
#endif

static uint8_t  u2_memory[W5100_MEM_SIZE];
static uint8_t  u2_mode_register;
static uint16_t u2_data_address;

typedef struct {
  uint16_t transmit_base;
  uint16_t transmit_size;
  uint16_t receive_base;
  uint16_t receive_size;
  uint16_t register_address;
  /* RX producer: monotonic byte counter; mask only for ring indexing (§10l). */
  uint16_t sn_rx_wr;
  /* Atomic, always-complete shadow of the host-visible Sn_RX_RD (consumer pointer).
   * The 6502 driver updates Sn_RX_RD as two byte stores (hi @ $x28, then lo @ $x29) on core 1;
   * core 0 must never observe the half-updated new_hi:old_lo value or it will over-estimate the
   * consumed region and overwrite unread ring bytes. We publish this shadow atomically when the
   * host completes the low byte (see write_socket_register) and core 0 reads it here (§1cf). */
  uint16_t sn_rx_rd;
} u2_socket_t;

static u2_socket_t u2_sockets[W5100_NUM_SOCKETS];

/* Sn_IR and common IR are write-1-to-clear shadows. Core 0 ORs bits; core 1 reads them. */
static uint8_t u2_sn_ir[W5100_NUM_SOCKETS];
static uint8_t u2_ir;
static uint8_t u2_host_net_locked;
static volatile uint8_t u2_cmd_epoch;

enum {
  U2_DF_NONE = 0,
  U2_DF_OPEN,
  U2_DF_CONNECT,
  U2_DF_LISTEN,
  U2_DF_DISCON,
  U2_DF_CLOSE,
  U2_DF_SEND,
};

typedef struct {
  uint8_t op;
  uint8_t mr;
  uint8_t proto;
  uint16_t port;
  uint16_t dport;
  uint32_t dip;
  uint16_t tx_rd;
  uint16_t tx_wr;
  uint16_t sent;
} u2_defer_t;

static u2_defer_t u2_defer[W5100_NUM_SOCKETS];
#if UTHERNET2_DEBUG
static uint16_t u2_dbg_last_wire[W5100_NUM_SOCKETS];
static uint8_t u2_dbg_stall_dumped[W5100_NUM_SOCKETS];
#endif

enum {
  U2_RX_PROTO_UDP = 1,
  U2_RX_PROTO_TCP = 2,
  U2_RX_PROTO_MACRAW = 3,
};

enum {
  U2_RX_DROP_NO_ROOM = 1,
  U2_RX_DROP_PARTIAL = 2,
  U2_RX_DROP_FRAME_TOO_BIG = 3,
  U2_RX_DROP_SIZE_CLAMPED = 4,
};

static inline uint8_t get_byte(uint16_t val, unsigned shift) {
  return (uint8_t)((val >> shift) & 0xFF);
}

static uint16_t U2_BUS_RAM(read_net16)(const uint8_t *p) {
  return (uint16_t)p[0] << 8 | p[1];
}

static inline uint16_t u2_rx_wr_load(const u2_socket_t *s) {
  return __atomic_load_n(&s->sn_rx_wr, __ATOMIC_ACQUIRE);
}

static inline void u2_rx_wr_store(u2_socket_t *s, uint16_t wr) {
  __atomic_store_n(&s->sn_rx_wr, wr, __ATOMIC_RELEASE);
}

/* Reset a socket's RX/TX ring pointers to a coherent empty state (RSR=0, FSR=full).
 * A real W5100 initializes the socket's internal pointers on the OPEN command, so the driver
 * sees Sn_RX_RSR=0 on a freshly opened socket. We had never done this: the FIRST OPEN after a
 * chip reset was fine (pointers already 0), but a SECOND OPEN that reused a socket (e.g. Contiki
 * opening MACRAW socket 0 after a prior telnet session) left sn_rx_wr at the previous session's
 * offset. Sn_RX_RSR = wr - rd was then non-zero, so ip65's poll() "received" a garbage frame,
 * advanced Sn_RX_RD by a bogus length header, and RSR never converged to 0 -> an unbounded
 * "sock0 RECV" storm and no DNS/connection progress. Zeroing both producer and consumer on OPEN
 * matches hardware and stops the storm. */
static void u2_reset_socket_rings(int i) {
  u2_socket_t *s = &u2_sockets[i];
  uint16_t reg = s->register_address;
  u2_rx_wr_store(s, 0);
  __atomic_store_n(&s->sn_rx_rd, 0, __ATOMIC_RELEASE);
  u2_memory[reg + W5100_SN_RX_RD0] = 0;
  u2_memory[reg + W5100_SN_RX_RD1] = 0;
  u2_memory[reg + W5100_SN_TX_RD0] = 0;
  u2_memory[reg + W5100_SN_TX_RD1] = 0;
  u2_memory[reg + W5100_SN_TX_WR0] = 0;
  u2_memory[reg + W5100_SN_TX_WR1] = 0;
#if UTHERNET2_DEBUG
  u2_dbg_stall_dumped[i] = 0;
#endif
}

static inline uint16_t u2_rx_rd_load(const u2_socket_t *s) {
  return __atomic_load_n(&s->sn_rx_rd, __ATOMIC_ACQUIRE);
}

/* Unread bytes in emulated RX ring. ip65 keeps Sn_RX_RD as a physical W5100 address;
 * mask extracts ring offset when receive_base is size-aligned (RMSR layout).
 * rd is read from the atomic shadow (never the raw u2_memory byte pair) to avoid the
 * cross-core new_hi:old_lo tear that let core 0 overwrite unread bytes (§1cf). */
static uint16_t u2_rx_used_bytes(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t size = s->receive_size;
  if (size == 0)
    return 0;
  uint16_t mask = size - 1;
  uint16_t rd_off = (uint16_t)(u2_rx_rd_load(s) & mask);
  uint16_t wr_off = (uint16_t)(u2_rx_wr_load(s) & mask);
  int d = (int)wr_off - (int)rd_off;
  if (d < 0)
    d += (int)size;
  return (uint16_t)d;
}

/* Host-facing occupancy (Sn_RX_RSR). MUST read the LIVE Sn_RX_RD from u2_memory, not the core-0
 * shadow: this runs on core 1 (the same core that writes Sn_RX_RD), so it is coherent, and it lets
 * RSR shrink AS the host advances Sn_RX_RD between frame reads. §1ch published the shadow only at
 * the RECV command, so a driver that reads several frames per RECV (Contiki under load) saw RSR
 * stay high while it drained, kept reading PAST Sn_RX_WR into stale ring bytes, mis-parsed a length
 * header, and stalled Sn_RX_RD → the unbounded "sock0 RECV" storm (§1cj). Core 0's producer free-
 * space math still uses the tear-free shadow (u2_rx_used_bytes) so it can never overwrite unread
 * data (shadow ≤ live rd ⇒ conservative). */
static uint16_t U2_BUS_RAM(u2_rx_used_bytes_live)(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t size = s->receive_size;
  if (size == 0)
    return 0;
  uint16_t mask = size - 1;
  uint16_t ra = s->register_address;
  uint16_t rd = (uint16_t)(((uint16_t)u2_memory[ra + W5100_SN_RX_RD0] << 8)
                           | u2_memory[ra + W5100_SN_RX_RD1]);
  uint16_t rd_off = (uint16_t)(rd & mask);
  uint16_t wr_off = (uint16_t)(u2_rx_wr_load(s) & mask);
  int d = (int)wr_off - (int)rd_off;
  if (d < 0)
    d += (int)size;
  return (uint16_t)d;
}

static volatile uint32_t u2_bus_n;

void U2_RxDebugProbe(int socket_i, int *cr, int *bus_n, int *rx_rd) {
  if (!cr || !bus_n || !rx_rd)
    return;
  *bus_n = (int)u2_bus_n;
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS) {
    *cr = *rx_rd = -1;
    return;
  }
  uint16_t ra = u2_sockets[socket_i].register_address;
  *cr = (int)u2_memory[ra + W5100_SN_CR];
  *rx_rd = (int)(((uint16_t)u2_memory[ra + W5100_SN_RX_RD0] << 8)
                 | u2_memory[ra + W5100_SN_RX_RD1]);
}

void U2_RxDebugStat(int socket_i, int *size, int *live_rsr, int *shadow_used) {
  if (!size || !live_rsr || !shadow_used)
    return;
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS) {
    *size = *live_rsr = *shadow_used = -1;
    return;
  }
  *size = (int)u2_sockets[socket_i].receive_size;
  *live_rsr = (int)u2_rx_used_bytes_live(socket_i);
  *shadow_used = (int)u2_rx_used_bytes(socket_i);
}

#if U2_RX_AUDIT
/* #region agent log
 * H7 vs H9, measured with one comparison each on paths that already compare the address.
 *
 * u2_aud_ring_end is socket 0's first address past the ring (receive_base + receive_size), cached
 * so auto_increment costs a single compare against a global rather than a struct walk.
 *
 * hit_end : auto_increment carried the address off the end of the ring. If this tracks the number
 *           of wrapping frames, the host really does read past the end (H7).
 * repoint : the host wrote an address register while the address was already past the ring end,
 *           i.e. it noticed and re-pointed. Counted in the *write* path, off the read hot path.
 *
 * The distinction decides the fix. hit_end>0 with repoint==0 means the host consumed 2 foreign
 * bytes and the ring must wrap at the socket boundary. hit_end>0 *with* repoint>0 means the host
 * re-pointed and discarded them, so the 2-byte deficit is benign and the checksum fault is
 * elsewhere (H9) — in which case wrapping would duplicate 2 bytes and make things worse. */
static uint16_t u2_aud_ring_end;
volatile uint32_t g_u2_aud_hit_end, g_u2_aud_repoint;

/* §1dl. The re-point target came back as ring_base + 0 on 3 of 3 captures, which kills the
 * one-line "wrap the address at the ring end" fix: the host would read the first 2 wrapped bytes
 * at the boundary and then re-read them after re-pointing, duplicating 2 bytes instead of losing
 * 2. Counters cannot separate the remaining candidates, so trace the pointer instead.
 *
 * Every address-register write is recorded as (address, total reads so far). The gap in the read
 * count between consecutive entries is how many bytes the host pulled from that pointer position,
 * so the pair reconstructs the exact access pattern with nothing added to the read hot path.
 *
 * The buffer overwrites freely until the address reaches the ring end, then records U2_TRC_TAIL
 * more entries and freezes — so the dump straddles one real wrap with its lead-in intact, rather
 * than showing the unrelated first 32 writes after boot.
 *
 * §1dm: BOTH address registers are traced now, tagged by U2_TRC_HIGH. Tracing only the low write
 * left the trace blind to a HIGH-only re-point — and that is precisely how a driver wraps 0x7000
 * back to 0x6000, since the low byte is already zero and only the high byte needs to change. An
 * invisible re-point mid-burst would make a run of reads look contiguous when it is not, which is
 * enough to invalidate the read-count arithmetic entirely. Doubled the depth to keep the same
 * number of pointer moves in view. */
#define U2_TRC_MAX 64u
#define U2_TRC_TAIL 20u
#define U2_TRC_HIGH 0x10000u
/* §1dn: paired with each entry, the LIVE Sn_RX_RD the host can see at that instant. Both captured
 * wraps split as if the record began 2 bytes before the pointer the host had just written, so the
 * question is whether our Sn_RX_RD agrees with that pointer or trails it by 2. Equal means the
 * driver's split is its own arithmetic; short by 2 means our RD lags the record start and the
 * defect is ours. Read straight out of u2_memory because that is exactly what the host reads. */
/* §1do: Sn_RX_RSR alongside Sn_RX_RD. ip65's w5100_data_request returns
 * MIN(Sn_RX_RSR, addr_limit - addr), so the chunk the driver reads before it must re-point is a
 * function of RSR and RSR alone once the limit is fixed — and RSR is the last value the driver
 * reads that has never been measured. Computed live because RSR is derived, not stored. */
static volatile uint16_t u2_trc_rsr[U2_TRC_MAX];
static volatile uint16_t u2_trc_rd[U2_TRC_MAX];
static volatile uint32_t u2_trc[U2_TRC_MAX];
static volatile uint32_t u2_trc_w;      /* free-running write index; & (U2_TRC_MAX-1) to index */
static volatile uint32_t u2_trc_freeze; /* 0 = running, else entries left before freezing */
static volatile uint8_t u2_trc_done;
static uint8_t u2_trc_dumped; /* core 0 only */

static uint32_t u2_audit_reads; /* defined with the audit counters below */

/* Bus (write) path only — never reached from a read. */
static void U2_BUS_RAM(u2_trc_note)(uint32_t kind) {
  if (u2_trc_done)
    return;
  uint32_t w = u2_trc_w & (U2_TRC_MAX - 1u);
  u2_trc[w] = ((uint32_t)(u2_audit_reads & 0x7FFFu) << 17) | kind | u2_data_address;
  u2_trc_rd[w] = (uint16_t)(((uint16_t)u2_memory[0x0400 + W5100_SN_RX_RD0] << 8)
                            | u2_memory[0x0400 + W5100_SN_RX_RD1]);
  u2_trc_rsr[w] = u2_rx_used_bytes_live(0);
  u2_trc_w++;
  if (u2_trc_freeze && --u2_trc_freeze == 0u)
    u2_trc_done = 1;
}
/* #endregion */
#endif

static inline uint16_t u2_size_from_rmsr_field(uint8_t field) {
  return (uint16_t)(1u << (10 + (field & 3u))); /* 1K,2K,4K,8K */
}

static void u2_apply_socket_sizes(int is_rx, uint8_t value) {
  uint16_t base = is_rx ? W5100_RX_BASE : W5100_TX_BASE;
  const uint16_t end = is_rx ? W5100_MEM_SIZE : W5100_RX_BASE;
  uint8_t val = value;
  for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
    uint16_t requested = u2_size_from_rmsr_field(val);
    uint16_t assigned = requested;
    if (base + assigned > end) {
      assigned = (uint16_t)(end - base);
      if (is_rx) {
        U2_MonNetRxDrop(i, U2_RX_PROTO_MACRAW, U2_RX_DROP_SIZE_CLAMPED, requested, assigned, 0,
                        (uint16_t)(W5100_MEM_SIZE - W5100_RX_BASE));
      }
    }
    if (is_rx) {
      u2_socket_t *sock = &u2_sockets[i];
      uint16_t old_wr = u2_rx_wr_load(sock);
      sock->receive_base = base;
      sock->receive_size = assigned;
      /* Do not zero sn_rx_wr on every RMSR write — that desyncs RX vs Sn_RX_RD and breaks TCP/ACK progress.
       * Remap the producer offset into the new window; full chip reset clears pointers in u2_reset(). */
      if (assigned == 0) {
        u2_rx_wr_store(sock, 0);
      } else {
        u2_rx_wr_store(sock, (uint16_t)(old_wr % assigned));
      }
    } else {
      u2_sockets[i].transmit_base = base;
      u2_sockets[i].transmit_size = assigned;
    }
    base = (uint16_t)(base + assigned);
    val >>= 2;
  }
#if U2_RX_AUDIT
  /* #region agent log — cache socket 0's ring end so auto_increment needs one compare, not a
   * struct walk, on the ~90 ns read path. */
  if (is_rx)
    u2_aud_ring_end = (uint16_t)(u2_sockets[0].receive_base + u2_sockets[0].receive_size);
  /* #endregion */
#endif
}

static void u2_reset(void) {
#if UTHERNET2_DEBUG && U2_IP65_TRACE_DATA
  u2_ip65_data_trace_left = 0;
#endif
  U2_MonReset();
  __atomic_fetch_add(&u2_cmd_epoch, 1, __ATOMIC_ACQ_REL);
  u2_host_net_locked = 0;
  __atomic_store_n(&u2_ir, 0, __ATOMIC_RELEASE);
  for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
    __atomic_store_n(&u2_sn_ir[i], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&u2_defer[i].op, 0, __ATOMIC_RELEASE);
    U2_Net_Close(i);
  }
  memset(u2_memory, 0, sizeof(u2_memory));
  u2_mode_register = 0;
  /* data_address is NOT reset on soft reset (Uthernet II doc) */
  for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
    u2_sockets[i].transmit_base = 0;
    u2_sockets[i].transmit_size = 0;
    u2_sockets[i].receive_base = 0;
    u2_sockets[i].receive_size = 0;
    u2_sockets[i].register_address = (uint16_t)(W5100_S0_BASE + (i << 8));
    u2_sockets[i].sn_rx_wr = 0;
    u2_sockets[i].sn_rx_rd = 0;
  }
  /* RTR/RCR: ip65 w5100.s probes $0017/$0018 with XOR; must match or init returns SEC → "Device not found". */
  u2_memory[W5100_RTR0] = 0x07;
  u2_memory[W5100_RTR1] = 0xD0;
  u2_memory[W5100_RCR]  = 0x08;
  u2_memory[W5100_PTIMER] = 0x28;
  u2_memory[W5100_IMR] = W5100_IMR_SOCKETS;
  /* SHAR: same default MAC as ip65 drivers/w5100.s (WIZnet OUI). When RMSR==0x06, ip65 skips SW reset
   * (which writes SHAR) but still reads SHAR back into cfg_mac — without this, MAC is all zeros. */
  u2_memory[W5100_SHAR0] = 0x00;
  u2_memory[W5100_SHAR0 + 1] = 0x08;
  u2_memory[W5100_SHAR0 + 2] = 0xDC;
  u2_memory[W5100_SHAR0 + 3] = 0xA2;
  u2_memory[W5100_SHAR0 + 4] = 0xA2;
  u2_memory[W5100_SHAR5] = 0xA2;
  for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
    uint16_t ra = u2_sockets[i].register_address;
    u2_memory[ra + W5100_SN_DHAR0] = 0xFF;
    u2_memory[ra + W5100_SN_DHAR1] = 0xFF;
    u2_memory[ra + W5100_SN_DHAR2] = 0xFF;
    u2_memory[ra + W5100_SN_DHAR3] = 0xFF;
    u2_memory[ra + W5100_SN_DHAR4] = 0xFF;
    u2_memory[ra + W5100_SN_DHAR5] = 0xFF;
    u2_memory[ra + W5100_SN_TTL]   = 0x80;
  }
  /* Default buffer sizes: 0x06 so ip65 chip-check path accepts without full reset; ip65 then writes 0x0A. */
  u2_memory[W5100_RMSR] = 0x06;
  u2_memory[W5100_TMSR] = 0x06;
  u2_apply_socket_sizes(0, 0x06);
  u2_apply_socket_sizes(1, 0x06);
}

static void U2_BUS_RAM(set_tx_sizes)(uint16_t address, uint8_t value) {
  u2_memory[address] = value;
  u2_apply_socket_sizes(0, value);
}

static void U2_BUS_RAM(set_rx_sizes)(uint16_t address, uint8_t value) {
  u2_memory[address] = value;
  u2_apply_socket_sizes(1, value);
}

static uint16_t U2_BUS_RAM(get_tx_data_size)(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t size = s->transmit_size;
  if (size == 0)
    return 0;
  uint16_t mask = size - 1;
  const uint8_t *r = &u2_memory[s->register_address];
  int rd = read_net16(r + W5100_SN_TX_RD0) & mask;
  int wr = read_net16(r + W5100_SN_TX_WR0) & mask;
  int data = wr - rd;
  if (data < 0) data += size;
  return (uint16_t)data;
}

static uint16_t U2_BUS_RAM(get_tx_fsr)(int i) {
  uint16_t ts = u2_sockets[i].transmit_size;
  if (ts == 0)
    return 0;
  /* Report the TRUE free TX space (W5100-accurate). §1cg previously capped MACRAW FSR at 1518
   * to stop >MTU queueing, but that made us lie about the register: a host that derives its
   * Sn_TX_WR from FSR then computed garbage pointers (telnet65: wild Sn_TX_WR high bytes 0xEE/
   * 0xEF ≈ 1518=0x05EE) and DNS never egressed (§1ci). Oversize is now handled defensively in
   * send_data (drop the desynced frame instead of lying about FSR). */
  return (uint16_t)(ts - get_tx_data_size(i));
}

/* W5100 RX occupancy: RSR = unread bytes in ring (§10l). Host-facing ⇒ LIVE rd (§1cj). */
static uint16_t U2_BUS_RAM(get_rx_rsr)(int i) {
  return u2_rx_used_bytes_live(i);
}

/* Per-byte live FSR/RSR (§1cp). §1cm latched on high-only: a low-first read returned latch=0
 * (FSR looked empty) until FSR0 was touched — first SYN/ARP delayed. Bramble lo-first have-flags
 * did not restore first-try connect. Recompute each access like pre-import firmware. */
static uint8_t U2_BUS_RAM(get_tx_fsr_byte)(int i, unsigned shift) {
  return get_byte(get_tx_fsr(i), shift);
}

static uint8_t U2_BUS_RAM(get_rx_rsr_byte)(int i, unsigned shift) {
  return get_byte(get_rx_rsr(i), shift);
}

/* §1cx: reached from read_value_at on every $C0C7 read AND every U2_PeekDataPort prefetch, so it
 * sits inside the ~90 ns window before the a2bus SM latches the next byte. Was XIP-resident and
 * contended with core 0's lwIP for the cache. */
static uint8_t U2_BUS_RAM(read_socket_register)(uint16_t address) {
  int i = (address >> 8) - 0x04;
  uint16_t loc = address & 0xFF;
  switch (loc) {
  case W5100_SN_MR:
  case W5100_SN_CR:
    return u2_memory[address];
  case W5100_SN_IR:
    return __atomic_load_n(&u2_sn_ir[i], __ATOMIC_ACQUIRE);
  case W5100_SN_SR:
    return U2_Net_GetStatus(i);
  case W5100_SN_TX_FSR0:
    return get_tx_fsr_byte(i, 8);
  case W5100_SN_TX_FSR1:
    return get_tx_fsr_byte(i, 0);
  case W5100_SN_TX_RD0:
    return u2_memory[address];
  case W5100_SN_TX_RD1:
    return u2_memory[address];
  case W5100_SN_TX_WR0:
    return u2_memory[address];
  case W5100_SN_TX_WR1:
    return u2_memory[address];
  case W5100_SN_RX_RSR0:
    return get_rx_rsr_byte(i, 8);
  case W5100_SN_RX_RSR1:
    return get_rx_rsr_byte(i, 0);
  case W5100_SN_RX_RD0:
  case W5100_SN_RX_RD1:
    return u2_memory[address];
  default:
    return u2_memory[address];
  }
}

static uint8_t U2_BUS_RAM(read_value_at)(uint16_t address) {
  if (address == W5100_MR)
    return u2_mode_register;
  if (address == W5100_IR)
    return __atomic_load_n(&u2_ir, __ATOMIC_ACQUIRE);
  if (address >= W5100_GAR0 && address <= W5100_UPORT1)
    return u2_memory[address];
  if (address >= W5100_S0_BASE && address <= W5100_S3_MAX)
    return read_socket_register(address);
  if (address >= W5100_TX_BASE && address <= W5100_MEM_MAX)
    return u2_memory[address];
  return u2_memory[address & W5100_MEM_MAX];
}

/* Peek runs after every $C0C4–$C0C7 cycle and is what the next $C0C7 read should return. */
static uint8_t u2_expect;
static uint8_t u2_expect_valid;
static volatile uint32_t u2_mis_n;
static volatile uint16_t u2_pic_at, u2_aud_at;
static volatile uint8_t u2_pic_byte, u2_aud_byte;
static volatile uint8_t u2_pic_ready, u2_aud_ready;
static volatile uint8_t u2_pic_got, u2_aud_got;
static volatile uint8_t u2_pic_seen, u2_aud_seen;
static volatile uint16_t u2_aim;
static volatile uint8_t u2_aim_seen;
/* Last slot cycles after the audio byte exists. Core 1 writes, core 0 reads once the bus has stopped. */
struct u2_cyc_s {
  uint16_t adr;
  uint8_t loc, rw, val, pk;
};
static struct u2_cyc_s u2_cyc[8];
static volatile uint8_t u2_cyc_i;

uint8_t U2_BUS_RAM(U2_PeekDataPort)(void) {
  /* Pair with core-0 release-store of sn_rx_wr so DATA peeks see the frame bytes. */
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  uint8_t v = read_value_at(u2_data_address);
  u2_expect = v;
  u2_expect_valid = 1;
  if (u2_cyc_i)
    u2_cyc[(u2_cyc_i - 1u) & 7u].pk = v;
  return v;
}

void U2_RxDebugQueue(int *waits, int *feeds, int *level) {
  /* a = times the data port returned a different memory byte than the previous peek.
   * b = cover byte read at the stored address (-1 if that address was never read).
   * c = first audio byte read (-1 if the player never read that address). */
  if (waits) *waits = (int)u2_mis_n;
  if (feeds) *feeds = u2_pic_seen ? (int)u2_pic_got : -1;
  if (level) *level = u2_aud_seen ? (int)u2_aud_got : -1;
}

void U2_RxDebugAim(int *aim, int *at, int *seen) {
  if (aim) *aim = u2_aim_seen ? (int)u2_aim : -1;
  if (at) *at = u2_aud_ready ? (int)u2_aud_at : -1;
  if (seen) *seen = (int)u2_aud_seen;
}

void U2_RxDebugCyc(int slot, int *adr, int *packed, int *pk) {
  uint8_t n = u2_cyc_i;
  if (slot < 0 || slot > 7 || n == 0) {
    if (adr) *adr = -1;
    if (packed) *packed = -1;
    if (pk) *pk = -1;
    return;
  }
  uint8_t idx = (uint8_t)((n - 1u - (uint8_t)slot) & 7u);
  const struct u2_cyc_s *e = &u2_cyc[idx];
  if (adr) *adr = e->adr;
  if (packed) *packed = ((int)e->loc << 16) | ((int)e->rw << 8) | e->val;
  if (pk) *pk = e->pk;
}

static void U2_BUS_RAM(auto_increment)(void) {
  if (u2_mode_register & W5100_MR_AI) {
    u2_data_address++;
    if (u2_data_address == W5100_RX_BASE || u2_data_address == W5100_MEM_SIZE)
      u2_data_address -= 0x2000;
#if U2_RX_AUDIT
    /* #region agent log — H7: did the address just walk off the end of socket 0's ring? Arms the
     * pointer trace so the dump captures a real wrap rather than the first writes after boot. */
    else if (u2_data_address == u2_aud_ring_end) {
      g_u2_aud_hit_end++;
      if (!u2_trc_freeze && !u2_trc_done)
        u2_trc_freeze = U2_TRC_TAIL;
    }
    /* #endregion */
#endif
  }
}

#if U2_RX_AUDIT
volatile uint32_t g_u2_audit_frames;      /* RECVs seen on socket 0 */
volatile uint32_t g_u2_audit_ok;          /* reads observed == Sn_RX_RD advance */
volatile uint32_t g_u2_audit_short;       /* 0 < observed < advance  => DROPPED CYCLES (H1) */
volatile uint32_t g_u2_audit_over;        /* observed > advance      => re-reads / stale addr */
volatile uint32_t g_u2_audit_skip;        /* observed == 0 < advance => driver discarded frame */
volatile uint32_t g_u2_audit_lost_bytes;  /* total deficit across all "short" frames */
volatile int32_t  g_u2_audit_last_delta;  /* observed - advance, most recent mismatch */
volatile uint32_t g_u2_audit_deficit[9];  /* histogram of deficits 1..8, [0] = 9+ */
volatile uint32_t g_u2_audit_wrapped;     /* mismatches whose consumed range crossed the ring end (H6) */
/* Core 1 exclusively: the bus path increments it, the RECV handler reads it, and the pointer
 * trace snapshots it. Core 0 only ever sees it already packed into u2_trc, so it does not need
 * to be volatile — and dropping volatile keeps the increment off the hot path's load/store
 * round trip (§1dl). Tentatively declared above for u2_trc_note. */

/* Per-mismatch detail. printf() must never run on the bus path, so core 1 only fills this ring
 * and core 0 drains it in U2_RxAuditReport (§1di). */
#define U2_AUDIT_EVT_MAX 24u
typedef struct {
  uint16_t prev_rd, new_rd, advance, seen, rsr, hdr;
  uint8_t kind;    /* 1 = short (dropped cycles), 2 = over, 3 = skip */
  uint8_t wrapped; /* consumed range crossed receive_base + receive_size */
} u2_audit_evt_t;
static u2_audit_evt_t u2_audit_evt[U2_AUDIT_EVT_MAX];
static volatile uint32_t u2_audit_evt_w, u2_audit_evt_r;

/* H4: core-0 starvation. If enabling UART stdio in a Release build turned previously discarded
 * printf()s into blocking 115200 writes, core 0 stops servicing lwIP for long stretches and
 * Contiki shows exactly the reported "block of data, stall, update" pattern. */
volatile uint32_t g_u2_core0_gap_max_us, g_u2_core0_gap_5ms, g_u2_core0_gap_20ms,
                  g_u2_core0_gap_100ms, g_u2_core0_polls;

/* H8 control: how many frames crossed the ring end in total, matched or not. If every wrapping
 * frame is short, the fault is structural; if only some are, it is a race. */
volatile uint32_t g_u2_audit_wrap_total;

/* Count every real host $C0C7 read. The prefetch path (U2_PeekDataPort -> read_value_at)
 * deliberately bypasses this, so peeks are never counted.
 *
 * §1dl dropped the old socket-window test because it hid reads *past* the ring end — the exact
 * quantity under investigation. But counting unconditionally swept in the Sn_RX_RSR / Sn_RX_RD /
 * Sn_SR reads the driver does between frames, which put every frame ~5 over and made the
 * seen-vs-advance comparison useless (122 of 122 "over").
 *
 * §1dm keeps both properties with one compare against a constant: the whole 8 KiB RX block starts
 * at W5100_RX_BASE, so this counts socket 0's ring *and* the region past its end at 0x7000, while
 * excluding the register file below 0x6000. No struct load, so it is still cheaper than the
 * original windowed form. */
static inline void u2_audit_note_read(uint16_t addr) {
  if (addr >= W5100_RX_BASE)
    u2_audit_reads++;
}
#endif

static uint8_t U2_BUS_RAM(read_value)(void) {
  uint16_t rd_addr = u2_data_address;
#if U2_RX_AUDIT
  u2_audit_note_read(rd_addr);
#endif
#if UTHERNET2_DEBUG
  /* Bisect ip65 init: move U2_IP65_CHECKPOINT between builds (CMake). */
  if (rd_addr == W5100_RTR0)
    U2_MonCheckpoint(2);
  else if (rd_addr == W5100_RTR1)
    U2_MonCheckpoint(3);
  else if (rd_addr == W5100_RMSR)
    U2_MonCheckpoint(4);
#endif
  uint8_t v = read_value_at(rd_addr);
  if (u2_expect_valid && rd_addr >= W5100_TX_BASE && v != u2_expect)
    u2_mis_n++;
  u2_expect_valid = 0;
  if (!u2_pic_seen && __atomic_load_n(&u2_pic_ready, __ATOMIC_ACQUIRE) &&
      rd_addr == u2_pic_at) {
    u2_pic_got = v;
    u2_pic_seen = 1;
  }
  if (!u2_aud_seen && __atomic_load_n(&u2_aud_ready, __ATOMIC_ACQUIRE) &&
      rd_addr == u2_aud_at) {
    u2_aud_got = v;
    u2_aud_seen = 1;
  }
#if UTHERNET2_DEBUG && U2_IP65_TRACE_DATA
  if (u2_ip65_data_trace_left > 0) {
    U2_MonDataReadTrace(rd_addr, v, u2_mode_register);
    u2_ip65_data_trace_left--;
  }
#endif
  auto_increment();
  return v;
}

static void U2_BUS_RAM(write_common_register)(uint16_t address, uint8_t value) {
  if (address == W5100_MR) {
    if (value & W5100_MR_RST)
      u2_reset();
    else {
      u2_mode_register = value;
#if UTHERNET2_DEBUG
      if (value == 0x03)
        U2_MonCheckpoint(1);
#endif
#if UTHERNET2_DEBUG && U2_IP65_TRACE_DATA
      u2_arm_ip65_data_trace(value);
#endif
    }
    return;
  }
  if ((address >= W5100_GAR0 && address <= W5100_GAR3) ||
      (address >= W5100_SUBR0 && address <= W5100_SUBR3) ||
      (address >= W5100_SHAR0 && address <= W5100_SHAR5) ||
      (address >= W5100_SIPR0 && address <= W5100_SIPR3)) {
    if (address <= W5100_SUBR3 || address >= W5100_SIPR0)
      u2_host_net_locked = 1;
    u2_memory[address] = value;
  }
  else if (address == W5100_IR) {
    uint8_t cur = __atomic_load_n(&u2_ir, __ATOMIC_ACQUIRE);
    __atomic_store_n(&u2_ir, (uint8_t)(cur & (uint8_t)~value), __ATOMIC_RELEASE);
  }
  else if (address == W5100_IMR) {
    u2_memory[address] = value;
  }
  else if (address == W5100_RMSR)
    set_rx_sizes(address, value);
  else if (address == W5100_TMSR)
    set_tx_sizes(address, value);
  else if (address >= W5100_GAR0 && address <= W5100_UPORT1)
    /* RTR/RCR/PTIMER, gaps $0013–$0016, etc.: must persist like real W5100 (was no-op). */
    u2_memory[address] = value;
}

/* Push received data into socket i's RX buffer.
 * UDP: write 4B IP + 2B port + 2B len (big-endian) then payload atomically.
 * TCP: payload only, allowing partial enqueue for backpressure.
 * Returns accepted payload bytes. */
static uint16_t u2_push_rx(int socket_i, const uint8_t *data, uint16_t len, int is_udp, uint32_t src_ip, uint16_t src_port) {
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS || !data) return 0;
  u2_socket_t *s = &u2_sockets[socket_i];
  uint16_t size = s->receive_size;
  if (size == 0) return 0;
  uint16_t mask = size - 1;
  uint16_t base = s->receive_base;
  uint16_t used = u2_rx_used_bytes(socket_i);
  uint16_t free_bytes = size - used;
  uint16_t accept_len = len;
  uint16_t total = len;
  uint16_t hdr = 0;
  if (is_udp == 1)
    hdr = 8;
  else if (is_udp == 2)
    hdr = 6;
  if (hdr) {
    total = (uint16_t)(len + hdr);
    if (free_bytes <= total) {
      used = u2_rx_used_bytes(socket_i);
      free_bytes = size - used;
      if (free_bytes <= total) {
        U2_MonNetRxDrop(socket_i, is_udp == 1 ? U2_RX_PROTO_UDP : U2_RX_PROTO_TCP,
                        U2_RX_DROP_NO_ROOM, len, 0, free_bytes, size);
        return 0;
      }
    }
  } else {
    /* Reserve one byte (see MACRAW note in u2_push_rx_macraw): keep >=1 byte free so used
     * never reaches size, which would read back as an empty ring. */
    if (free_bytes <= 1) {
      used = u2_rx_used_bytes(socket_i);
      free_bytes = size - used;
      if (free_bytes <= 1) {
        U2_MonNetRxDrop(socket_i, U2_RX_PROTO_TCP, U2_RX_DROP_NO_ROOM, len, 0, free_bytes, size);
        return 0;
      }
    }
    uint16_t usable = (uint16_t)(free_bytes - 1);
    if (accept_len > usable) accept_len = usable;
    total = accept_len;
    if (accept_len < len)
      U2_MonNetRxDrop(socket_i, U2_RX_PROTO_TCP, U2_RX_DROP_PARTIAL, len, accept_len, free_bytes, size);
  }
  uint16_t wr = u2_rx_wr_load(s);
  if (is_udp == 1 || is_udp == 2) {
    u2_memory[base + (wr & mask)] = (uint8_t)(src_ip >> 24);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)(src_ip >> 16);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)(src_ip >> 8);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)src_ip;
    wr++;
    if (is_udp == 1) {
      u2_memory[base + (wr & mask)] = (uint8_t)(src_port >> 8);
      wr++;
      u2_memory[base + (wr & mask)] = (uint8_t)src_port;
      wr++;
    }
    u2_memory[base + (wr & mask)] = (uint8_t)(len >> 8);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)len;
    wr++;
  }
  static uint32_t u2_tcp_n;
  static uint8_t u2_prev, u2_sig_done, u2_visu_done, u2_aud_done;
  static uint16_t u2_sig = 0xFFFFu;
  for (uint16_t k = 0; k < accept_len; k++) {
    uint8_t b = data[k];
    uint16_t at = (uint16_t)(base + (wr & mask));
    u2_memory[at] = b;
    wr++;
    if (!hdr && socket_i == 0) {
      uint32_t seq = u2_tcp_n++;
      if (!u2_sig_done && u2_prev == 0xA2 && b == 0x01 && seq < 4000u) {
        u2_sig = (uint16_t)(seq - 1u);
        u2_sig_done = 1;
        // #region agent log
        U2_AgentLog("H21", "sig", (int)u2_sig, u2_prev, b, 1);
        // #endregion
      }
      if (u2_sig_done && !u2_pic_ready && seq == (uint32_t)u2_sig + 2u) {
        u2_pic_byte = b;
        u2_pic_at = at;
        __atomic_store_n(&u2_pic_ready, 1, __ATOMIC_RELEASE);
        // #region agent log
        U2_AgentLog("H27", "pic", (int)at, b, 0, 1);
        // #endregion
      }
      if (u2_sig_done && !u2_visu_done &&
          seq == (uint32_t)u2_sig + 2u + 16384u) {
        u2_visu_done = 1;
        uint8_t b1 = (uint16_t)(k + 1u) < accept_len ? data[k + 1u] : 0;
        uint8_t b2 = (uint16_t)(k + 2u) < accept_len ? data[k + 2u] : 0;
        // #region agent log
        U2_AgentLog("H21", "visu", b, b1, b2, 1);
        // #endregion
      }
      /* First audio sample: 140 templates * 40 bytes after the hires pages.
       * Legal PWM bytes are 0x40-0x63 or 0x64-0x87. A zero here is a hole. */
      if (u2_sig_done && !u2_aud_done &&
          seq == (uint32_t)u2_sig + 2u + 16384u + 5600u) {
        u2_aud_done = 1;
        uint8_t b1 = (uint16_t)(k + 1u) < accept_len ? data[k + 1u] : 0;
        uint8_t b2 = (uint16_t)(k + 2u) < accept_len ? data[k + 2u] : 0;
        // #region agent log
        U2_AgentLog("H22", "aud", b, b1, b2, 1);
        // #endregion
        u2_aud_byte = b;
        u2_aud_at = at;
        __atomic_store_n(&u2_aud_ready, 1, __ATOMIC_RELEASE);
      }
      u2_prev = b;
    }
  }
  u2_rx_wr_store(s, wr);
  if (accept_len > 0 || hdr)
    U2_SocketIrq(socket_i, W5100_SN_IR_RECV);
  return accept_len;
}

/* Advance Sn_RX_RD to sn_rx_wr (discard unread). Not W5100-accurate; optional MACRAW compat only. */
static void u2_socket_discard_rx(int socket_i) {
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS) return;
  u2_socket_t *s = &u2_sockets[socket_i];
  uint16_t size = s->receive_size;
  if (size == 0) return;
  uint16_t mask = size - 1;
  uint16_t physical_rd = (uint16_t)(s->receive_base + (u2_rx_wr_load(s) & mask));
  uint16_t ra = s->register_address;
  u2_memory[ra + W5100_SN_RX_RD0] = (uint8_t)(physical_rd >> 8);
  u2_memory[ra + W5100_SN_RX_RD1] = (uint8_t)(physical_rd & 0xFF);
  __atomic_store_n(&s->sn_rx_rd, physical_rd, __ATOMIC_RELEASE); /* keep core-0 shadow coherent (§1cf) */
}

/* Compile-time escape hatch for the §1dp wrap-compatibility layout below. Set to 0 to store
 * records strictly the way a real W5100 would, which is correct against the datasheet but leaves
 * ip65/Contiki corrupting one frame per RX ring wrap. */
#ifndef U2_MACRAW_WRAP_COMPAT
#define U2_MACRAW_WRAP_COMPAT 1
#endif

/* True when parking 2 record bytes just past socket i's RX ring cannot be observed as another
 * socket's live RX. RMSR 0x06 gives sock0 4 KiB at 0x6000 and sock1 2 KiB at 0x7000, so the
 * spill addresses *are* inside sock1's assigned ring — but ip65/Contiki never OPEN sock1, and
 * the illegal host read is exactly those two bytes. Treating a CLOSED neighbour as occupying
 * the spill region made §1dq a no-op: every wrap used W5100-exact wrapping and checksums stayed
 * on the 1st/3rd/6th following packet. Skip CLOSED sockets; if a neighbour is actually open
 * over that region, fall back to datasheet layout. */
static int u2_rx_spill_is_free(int i) {
  const uint16_t end = (uint16_t)(u2_sockets[i].receive_base + u2_sockets[i].receive_size);
  if ((uint32_t)end + 1u >= (uint32_t)W5100_MEM_SIZE)
    return 0;
  for (int j = 0; j < W5100_NUM_SOCKETS; j++) {
    if (j == i || u2_sockets[j].receive_size == 0)
      continue;
    if (u2_memory[u2_sockets[j].register_address + W5100_SN_SR] == W5100_SN_SR_CLOSED)
      continue;
    if ((uint16_t)(end - u2_sockets[j].receive_base) < u2_sockets[j].receive_size)
      return 0;
  }
  return 1;
}

/* Push MACRAW frame: 2-byte length (big-endian) then frame data. */
static void u2_push_rx_macraw(int socket_i, const uint8_t *data, uint16_t len) {
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS || !data) return;
  u2_socket_t *s = &u2_sockets[socket_i];
  /* Honor the MACRAW MAC Filter (Sn_MR MF bit): a real W5100 in MACRAW+MF mode delivers only
   * frames addressed to its own MAC (SHAR) or broadcast. ip65/Contiki open socket 0 with
   * Sn_MR=0x44 and rely on this. Without it we copy every frame the STA sees (broadcast +
   * multicast + our unicast) into the small RX ring, which floods it with ambient traffic and
   * intermittently drops the unicast ARP reply / DNS response the host is waiting on. Dropping
   * filtered frames is normal hardware behavior (not an error), so do it silently. */
  if ((u2_memory[s->register_address + W5100_SN_MR] & W5100_SN_MR_MF) && len >= 6) {
    const uint8_t *dm = data; /* Ethernet destination MAC = first 6 bytes of the raw frame */
    int is_broadcast = (dm[0] & dm[1] & dm[2] & dm[3] & dm[4] & dm[5]) == 0xFF;
    int is_ours = dm[0] == u2_memory[W5100_SHAR0 + 0] && dm[1] == u2_memory[W5100_SHAR0 + 1] &&
                  dm[2] == u2_memory[W5100_SHAR0 + 2] && dm[3] == u2_memory[W5100_SHAR0 + 3] &&
                  dm[4] == u2_memory[W5100_SHAR0 + 4] && dm[5] == u2_memory[W5100_SHAR0 + 5];
    if (!is_broadcast && !is_ours) {
      return;
    }
  }
  uint16_t size = s->receive_size;
  if (size == 0) {
    return;
  }
  uint16_t mask = size - 1;
  uint16_t base = s->receive_base;
  uint16_t total = (uint16_t)(2 + len);
  uint16_t used = u2_rx_used_bytes(socket_i);
  uint16_t free_bytes = size - used;
  /* Reserve one byte: never let used reach size. wr_off==rd_off is otherwise ambiguous
   * (empty vs full) since RSR is derived from wr-rd, and "full" would read back as empty,
   * silently discarding a full ring during bulk RX (§1cf). Hence the >=/<= comparisons. */
  if (total >= size) {
    U2_MonNetRxDrop(socket_i, U2_RX_PROTO_MACRAW, U2_RX_DROP_FRAME_TOO_BIG, len, 0, free_bytes, size);
    return;
  }
  if (free_bytes <= total) {
#if U2_MACRAW_COMPAT_DROP_OLDEST
    /* Compatibility: flush unread once so DHCP/ARP bursts can progress (enable via -DU2_MACRAW_COMPAT_DROP_OLDEST=1). */
    u2_socket_discard_rx(socket_i);
    used = u2_rx_used_bytes(socket_i);
    free_bytes = size - used;
#endif
    if (free_bytes <= total) {
      used = u2_rx_used_bytes(socket_i);
      free_bytes = size - used;
    }
    if (free_bytes <= total) {
      U2_MonNetRxDrop(socket_i, U2_RX_PROTO_MACRAW, U2_RX_DROP_NO_ROOM, len, 0, free_bytes, size);
      return;
    }
  }
  uint16_t wr = u2_rx_wr_load(s);
  /* W5100 MACRAW RX length field is reported as frame_len + 2 in many drivers,
   * which then subtract 2 before reading frame bytes. */
  uint16_t wire_len = (uint16_t)(len + 2u);

  /* §1dp: place the record so the ip65/Contiki read pattern lands correctly across the ring end.
   *
   * That driver asks w5100_data_request() how much it may read before re-pointing and gets
   * MIN(Sn_RX_RSR, addr_limit - addr) = `first`. It then reads the 2-byte MACRAW header *plus*
   * `first` more, i.e. first+2 bytes from a window only `first` wide, so it runs 2 bytes past the
   * socket boundary; and it re-points to ring offset 0, a target computed from `first` rather than
   * from the first+2 bytes it actually consumed. Measured identically on four wraps
   * (burst = 2 + first, exact). Both halves are the driver's, not ours: our Sn_RX_RD, Sn_RX_RSR,
   * MACRAW header, producer wrap and auto-increment all match the datasheet and the independent
   * a2fpga core (§1dp), and no auto-increment behaviour can repair a displaced re-point target.
   *
   * So lay the two bytes it over-reads where it will actually look for them — just past the ring —
   * and start the remainder at offset 0 with the byte it expects there. Sn_RX_WR still advances by
   * the full record, so Sn_RX_RSR/Sn_RX_RD arithmetic is untouched and the next record still
   * begins at (off + total) & mask, exactly where the host's own Sn_RX_RD lands; the 2 ring bytes
   * between the tail and that point are simply skipped and never read.
   *
   * A *correct* driver would be broken by this, so it is confined to the case where nothing else
   * can observe the two bytes past the ring. */
  const uint16_t off0 = (uint16_t)(wr & mask);
  const uint16_t first = (uint16_t)(size - off0);
  const int spill = u2_rx_spill_is_free(socket_i);
  const int shim = U2_MACRAW_WRAP_COMPAT && total > first && spill;
  if (shim) {
    /* Contiguous run of first+2 bytes from off0, spilling 2 bytes past the ring end. */
    uint8_t *p = &u2_memory[base + off0];
    p[0] = (uint8_t)(wire_len >> 8);
    p[1] = (uint8_t)wire_len;
    for (uint16_t k = 0; k + 2u < (uint16_t)(first + 2u); k++)
      p[k + 2u] = data[k];
    /* Remainder from ring offset 0, starting at the byte the driver expects to find there. */
    for (uint16_t k = first; k < len; k++)
      u2_memory[base + (uint16_t)(k - first)] = data[k];
    wr = (uint16_t)(wr + total);
  } else {
    u2_memory[base + (wr & mask)] = (uint8_t)(wire_len >> 8);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)wire_len;
    wr++;
    for (uint16_t k = 0; k < len; k++) {
      u2_memory[base + (wr & mask)] = data[k];
      wr++;
    }
  }
  u2_rx_wr_store(s, wr);
#if UTHERNET2_DEBUG
  u2_dbg_last_wire[socket_i] = wire_len;
#endif
}

/* Read TX buffer data between rd and wr and send via network. Returns 0 on success, -1 if MACRAW not accepted. */
static int send_data(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t buf_size = s->transmit_size;
  if (buf_size == 0) return 0;
  uint16_t mask = buf_size - 1;
  const uint8_t *r = &u2_memory[s->register_address];
  uint16_t rd_full = read_net16(r + W5100_SN_TX_RD0);
  uint16_t wr_full = read_net16(r + W5100_SN_TX_WR0);
  uint16_t rd = rd_full & mask;
  uint16_t wr = wr_full & mask;
  int data_len = (int)wr - (int)rd;
  if (data_len < 0) data_len += buf_size;
  if (data_len == 0) return 0;
  uint16_t base = s->transmit_base;
  uint16_t consumed = (uint16_t)data_len; /* bytes to advance Sn_TX_RD past */
  uint8_t status = U2_Net_GetStatus(i);
  if (status == W5100_SN_SR_SOCK_UDP) {
    uint32_t dip = (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR0] << 24
                  | (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR1] << 16
                  | (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR2] << 8
                  | (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR3];
    uint16_t dport = (uint16_t)u2_memory[s->register_address + W5100_SN_DPORT0] << 8
                  | (uint16_t)u2_memory[s->register_address + W5100_SN_DPORT1];
    /* Item 3: send the whole datagram with no truncation and no large scratch buffer.
     * The net layer copies straight from the (possibly wrapping) TX ring into the pbuf. */
    U2_MonNetUdpSend(i, dip, dport, (uint16_t)data_len);
    U2_Net_SendUdp(i, &u2_memory[base], buf_size, rd, (uint16_t)data_len, dip, dport);
    consumed = (uint16_t)data_len; /* UDP datagram is atomic: always drain fully */
  } else if (status == W5100_SN_SR_ESTABLISHED) {
    /* Shared-access clients (e.g. wget65) can queue >2 KiB before SEND. Send in chunks and
     * respect lwIP backpressure: advance TX_RD only by bytes lwIP accepted. Any remainder
     * stays in the FIFO TX ring and flushes in order on the host's next SEND (no loss). */
    uint8_t buf[1024];
    int remaining = data_len;
    int pos = 0;
    U2_MonNetTcpSend(i, (uint16_t)data_len);
    while (remaining > 0) {
      int n = remaining;
      if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
      for (int j = 0; j < n; j++)
        buf[j] = u2_memory[base + ((rd + pos + j) & mask)];
      int acc = U2_Net_SendTcp(i, buf, (uint16_t)n);
      if (acc < 0)
        break; /* fatal socket error: stop, keep remainder in ring */
      pos += acc;
      remaining -= acc;
      if (acc < n)
        break; /* lwIP send buffer full: leave remainder queued */
    }
    consumed = (uint16_t)pos;
  } else if (status == W5100_SN_SR_SOCK_MACRAW) {
    uint8_t buf[1518];
    /* Log the true RD→WR span so oversize/desync is visible on UART (OVERSIZE tag if >1518). */
    U2_MonNetMacrawTxPtrs(i, (uint16_t)data_len, rd_full, wr_full, rd, wr);
    if (data_len > (int)sizeof(buf)) {
      /* data_len > one max Ethernet frame ⇒ host TX_WR/TX_RD desync (garbage Sn_TX_WR, e.g.
       * 0xEFxx). A real single MACRAW frame is ≤1518. Do NOT blast stale ring bytes onto the
       * wire — that emits a corrupt Ethernet frame that peers/switches drop (why the DNS server
       * never replied, §1ci). Drop this frame but still retire the full host TX window so the
       * pointers re-sync and Sn_TX_FSR recovers. */
      U2_MonNetMacrawTx(i, 0); /* len=0 paired with an OVERSIZE ptrs line == desync drop */
      consumed = (uint16_t)data_len;
    } else {
      int n = data_len;
      for (int j = 0; j < n; j++)
        buf[j] = u2_memory[base + ((rd + j) & mask)];
      /* Log TX only from core-0 linkoutput (`u2_send_macraw_core0`). A second line here made
       * every SEND look like a duplicate frame; the queue copies once and drains once. */
      if (U2_Net_SendMacraw(i, buf, (uint16_t)n) != 0) {
        return -1; /* not accepted: do NOT advance TX_RD, retry on next SEND */
      }
      consumed = (uint16_t)data_len;
    }
  }
  /* Advance TX_RD by the bytes actually consumed (full host-visible pointer progression;
   * only ring indexing is masked). */
  uint16_t new_rd = (uint16_t)(rd_full + consumed);
  u2_memory[s->register_address + W5100_SN_TX_RD0] = (uint8_t)(new_rd >> 8);
  u2_memory[s->register_address + W5100_SN_TX_RD1] = (uint8_t)new_rd;
  return 0;
}

/* §1cx: the RECV branch runs immediately before the host reads the next frame's length header,
 * so its latency lands directly on the prefetch deadline. Keep it in SRAM (RP2350 only). */
static void u2_cr_clear(int i) {
  u2_memory[u2_sockets[i].register_address + W5100_SN_CR] = 0;
}

static void u2_tx_rd_store(int i, uint16_t rd) {
  uint16_t ra = u2_sockets[i].register_address;
  u2_memory[ra + W5100_SN_TX_RD0] = (uint8_t)(rd >> 8);
  u2_memory[ra + W5100_SN_TX_RD1] = (uint8_t)rd;
}

static uint16_t u2_tx_span(int i, uint16_t rd_full, uint16_t wr_full) {
  uint16_t size = u2_sockets[i].transmit_size;
  if (size == 0)
    return 0;
  uint16_t mask = (uint16_t)(size - 1);
  int data = (int)(wr_full & mask) - (int)(rd_full & mask);
  if (data < 0)
    data += (int)size;
  return (uint16_t)data;
}

static int u2_epoch_ok(uint8_t ep) {
  return __atomic_load_n(&u2_cmd_epoch, __ATOMIC_ACQUIRE) == ep;
}

static void u2_defer_finish(int i, uint8_t ep) {
  __atomic_store_n(&u2_defer[i].op, U2_DF_NONE, __ATOMIC_RELEASE);
  if (u2_epoch_ok(ep))
    u2_cr_clear(i);
}

void U2_SocketIrq(int socket_i, uint8_t bits) {
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS || bits == 0)
    return;
  __atomic_fetch_or(&u2_sn_ir[socket_i], bits, __ATOMIC_ACQ_REL);
  uint8_t imr = u2_memory[W5100_IMR];
  if (imr & (uint8_t)(1u << socket_i))
    __atomic_fetch_or(&u2_ir, (uint8_t)(1u << socket_i), __ATOMIC_ACQ_REL);
}

void U2_MirrorStaNet(const uint8_t ip[4], const uint8_t gw[4], const uint8_t mask[4]) {
  if (u2_host_net_locked || !ip || !gw || !mask)
    return;
  for (int b = 0; b < 4; b++) {
    u2_memory[W5100_SIPR0 + b] = ip[b];
    u2_memory[W5100_GAR0 + b] = gw[b];
    u2_memory[W5100_SUBR0 + b] = mask[b];
  }
}

static void u2_defer_open(int i, u2_defer_t *d, uint8_t ep) {
  int ok = 0;
  switch (d->mr & W5100_SN_MR_PROTO_MASK) {
  case W5100_SN_MR_UDP:
    ok = (U2_Net_OpenUdp(i, d->port) == 0);
    break;
  case W5100_SN_MR_TCP:
    ok = (U2_Net_OpenTcp(i) == 0);
    if (ok)
      U2_Net_SetTcpNoDelay(i, (d->mr & W5100_SN_MR_ND) != 0);
    break;
  case W5100_SN_MR_IPRAW:
    ok = (U2_Net_OpenIpraw(i, d->proto) == 0);
    break;
  default:
    ok = 0;
    break;
  }
  if (!u2_epoch_ok(ep)) {
    U2_Net_Close(i);
    __atomic_store_n(&u2_defer[i].op, U2_DF_NONE, __ATOMIC_RELEASE);
    return;
  }
  U2_MonSockOpen(i, d->mr, d->port, ok);
  // #region agent log
  U2_AgentLog("H5", "open", i, d->mr, ok, 1);
  // #endregion
  /* a2stream polls Sn_SR for SOCK_INIT and then writes CONNECT without waiting
   * for Sn_CR. Publishing INIT while this OPEN still owns the defer slot drops
   * that CONNECT (arm-fail op 2) and the following CR clear makes it look done. */
  u2_defer_finish(i, ep);
  if (ok && u2_epoch_ok(ep)) {
    uint8_t st = W5100_SN_SR_CLOSED;
    switch (d->mr & W5100_SN_MR_PROTO_MASK) {
    case W5100_SN_MR_TCP:
      st = W5100_SN_SR_SOCK_INIT;
      break;
    case W5100_SN_MR_UDP:
      st = W5100_SN_SR_SOCK_UDP;
      break;
    case W5100_SN_MR_IPRAW:
      st = W5100_SN_SR_SOCK_IPRAW;
      break;
    default:
      break;
    }
    U2_Net_SetStatus(i, st);
  }
}

static void u2_defer_send(int i, u2_defer_t *d, uint8_t ep) {
  uint8_t st = U2_Net_GetStatus(i);
  if (st == W5100_SN_SR_SOCK_CLOSE_WAIT || st == W5100_SN_SR_SOCK_FIN_WAIT ||
      st == W5100_SN_SR_SOCK_LAST_ACK || st == W5100_SN_SR_CLOSED) {
    // #region agent log
    U2_AgentLog("H3", "send-bad-st", i, st, 0, 1);
    // #endregion
    U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
    u2_defer_finish(i, ep);
    return;
  }
  uint16_t total = u2_tx_span(i, d->tx_rd, d->tx_wr);
  if (d->sent >= total) {
    u2_tx_rd_store(i, d->tx_wr);
    U2_SocketIrq(i, W5100_SN_IR_SEND_OK);
    u2_defer_finish(i, ep);
    return;
  }
  uint16_t size = u2_sockets[i].transmit_size;
  uint16_t mask = (uint16_t)(size - 1);
  uint16_t base = u2_sockets[i].transmit_base;
  uint16_t left = (uint16_t)(total - d->sent);
  if (st == W5100_SN_SR_SOCK_UDP || st == W5100_SN_SR_SOCK_IPRAW) {
    uint16_t off = (uint16_t)(d->tx_rd & mask);
    int rc = (st == W5100_SN_SR_SOCK_UDP)
                 ? U2_Net_SendUdp(i, &u2_memory[base], size, off, left, d->dip, d->dport)
                 : U2_Net_SendIpraw(i, &u2_memory[base], size, off, left, d->dip);
    if (!u2_epoch_ok(ep)) {
      __atomic_store_n(&u2_defer[i].op, U2_DF_NONE, __ATOMIC_RELEASE);
      return;
    }
    if (rc != 0) {
      U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
      u2_defer_finish(i, ep);
      return;
    }
    u2_tx_rd_store(i, d->tx_wr);
    U2_SocketIrq(i, W5100_SN_IR_SEND_OK);
    u2_defer_finish(i, ep);
    return;
  }
  if (st != W5100_SN_SR_ESTABLISHED) {
    U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
    u2_defer_finish(i, ep);
    return;
  }
  uint8_t buf[256];
  uint16_t n = left > (uint16_t)sizeof(buf) ? (uint16_t)sizeof(buf) : left;
  uint16_t off = (uint16_t)((d->tx_rd + d->sent) & mask);
  for (uint16_t j = 0; j < n; j++)
    buf[j] = u2_memory[base + ((off + j) & mask)];
  int acc = U2_Net_SendTcp(i, buf, n);
  if (!u2_epoch_ok(ep)) {
    __atomic_store_n(&u2_defer[i].op, U2_DF_NONE, __ATOMIC_RELEASE);
    return;
  }
  if (acc < 0) {
    // #region agent log
    U2_AgentLog("H2", "send-fail", i, st, acc, 1);
    // #endregion
    U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
    u2_defer_finish(i, ep);
    return;
  }
  d->sent = (uint16_t)(d->sent + (uint16_t)acc);
  u2_tx_rd_store(i, (uint16_t)(d->tx_rd + d->sent));
  if (acc == 0 || d->sent < total) {
    // #region agent log
    U2_AgentLog("H2", "send-hold", i, (int)d->sent, (int)total, 0);
    // #endregion
    return; /* Sn_CR stays set; core 0 retries until lwIP accepts the span */
  }
  // #region agent log
  U2_AgentLog("H2", "send-ok", i, (int)total, st, 0);
  // #endregion
  U2_SocketIrq(i, W5100_SN_IR_SEND_OK);
  u2_defer_finish(i, ep);
}

static volatile uint8_t u2_arm_fail_sock;
static volatile uint8_t u2_arm_fail_op;
static volatile uint8_t u2_arm_fail_pending;

void U2_ProcessDeferredSocketCmds(void) {
  if (get_core_num() != 0)
    return;
  // #region agent log
  if (u2_arm_fail_pending) {
    U2_AgentLog("H6", "arm-fail", u2_arm_fail_sock, u2_arm_fail_op, 0, 1);
    u2_arm_fail_pending = 0;
  }
  // #endregion
  for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
    uint8_t op = __atomic_load_n(&u2_defer[i].op, __ATOMIC_ACQUIRE);
    if (op == U2_DF_NONE)
      continue;
    uint8_t ep = __atomic_load_n(&u2_cmd_epoch, __ATOMIC_ACQUIRE);
    u2_defer_t *d = &u2_defer[i];
    int rc;
    switch (op) {
    case U2_DF_OPEN:
      u2_defer_open(i, d, ep);
      break;
    case U2_DF_CONNECT:
      // #region agent log
      U2_AgentLog("H6", "conn-go", i, (int)d->dip, (int)d->dport, 1);
#ifndef PICO_RP2040
      /* H23 samples the sticky listener-drop bit. Clear it here so a later
       * 1 means a cycle was dropped after CONNECT, not at power-on. */
      pio0->fdebug = (1u << SM_LISTENER);
#endif
      // #endregion
      rc = U2_Net_ConnectTcpEx(i, d->dip, d->dport, d->port);
      if (!u2_epoch_ok(ep)) {
        U2_Net_Close(i);
        __atomic_store_n(&u2_defer[i].op, U2_DF_NONE, __ATOMIC_RELEASE);
        break;
      }
      U2_MonSockConnect(i, d->dip, d->dport, rc == 0);
      // #region agent log
      U2_AgentLog("H7", "conn-rc", i, rc, U2_Net_GetStatus(i), 1);
      // #endregion
      if (rc != 0) {
        U2_Net_Close(i);
        U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
      }
      u2_defer_finish(i, ep);
      break;
    case U2_DF_LISTEN:
      rc = U2_Net_ListenTcp(i, d->port);
      if (!u2_epoch_ok(ep)) {
        U2_Net_Close(i);
        __atomic_store_n(&u2_defer[i].op, U2_DF_NONE, __ATOMIC_RELEASE);
        break;
      }
      U2_MonSockListen(i, d->port, rc == 0);
      if (rc != 0) {
        U2_Net_Close(i);
        U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
      }
      u2_defer_finish(i, ep);
      break;
    case U2_DF_DISCON:
      rc = U2_Net_DisconTcp(i);
      if (!u2_epoch_ok(ep)) {
        __atomic_store_n(&u2_defer[i].op, U2_DF_NONE, __ATOMIC_RELEASE);
        break;
      }
      if (rc != 0)
        U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
      u2_defer_finish(i, ep);
      break;
    case U2_DF_CLOSE:
      U2_MonSockClose(i);
      U2_Net_Close(i);
      u2_defer_finish(i, ep);
      break;
    case U2_DF_SEND:
      u2_defer_send(i, d, ep);
      break;
    default:
      u2_defer_finish(i, ep);
      break;
    }
  }
}

static int u2_defer_arm(int i, const u2_defer_t *src) {
  if (__atomic_load_n(&u2_defer[i].op, __ATOMIC_ACQUIRE) != U2_DF_NONE) {
    // #region agent log
    u2_arm_fail_sock = (uint8_t)i;
    u2_arm_fail_op = src->op;
    u2_arm_fail_pending = 1;
    // #endregion
    return -1;
  }
  u2_defer[i].mr = src->mr;
  u2_defer[i].proto = src->proto;
  u2_defer[i].port = src->port;
  u2_defer[i].dport = src->dport;
  u2_defer[i].dip = src->dip;
  u2_defer[i].tx_rd = src->tx_rd;
  u2_defer[i].tx_wr = src->tx_wr;
  u2_defer[i].sent = 0;
  __atomic_store_n(&u2_defer[i].op, src->op, __ATOMIC_RELEASE);
  U2_RequestCore0NetPoll();
  return 0;
}

static void U2_BUS_RAM(write_socket_register)(uint16_t address, uint8_t value) {
  uint16_t loc = address & 0xFF;
  int sock_i = (address >> 8) - 0x04;
  if (loc == W5100_SN_IR && sock_i >= 0 && sock_i < W5100_NUM_SOCKETS) {
    uint8_t cur = __atomic_load_n(&u2_sn_ir[sock_i], __ATOMIC_ACQUIRE);
    __atomic_store_n(&u2_sn_ir[sock_i], (uint8_t)(cur & (uint8_t)~value), __ATOMIC_RELEASE);
    return;
  }
  u2_memory[address] = value;
  /* NOTE: Sn_RX_RD byte writes deliberately do NOT publish the core-0 shadow. On a real W5100
   * the host's Sn_RX_RD update only takes effect (Sn_RX_RSR is recomputed) when the RECV command
   * is issued; the shadow is therefore published in the RECV handler below, where both RX_RD bytes
   * are final. Publishing on the low-byte write (§1cf) assumed the driver writes RX_RD hi-then-lo
   * (ip65); Contiki writes lo-then-hi, so publishing on the low byte captured {old_hi:new_lo} and
   * never re-published the high byte — across a 256-byte boundary the shadow lagged by 256 and
   * over-reported Sn_RX_RSR, resurrecting the unbounded "sock0 RECV" storm (§1ch). */
  if (loc == W5100_SN_CR) {
    int i = (address >> 8) - 0x04;
    int hold_cr = 0;
    switch (value) {
    case W5100_SN_CR_OPEN: {
      uint8_t mr = u2_memory[(address & 0xFF00) + W5100_SN_MR];
      uint16_t port = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
                    | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
      /* Hardware resets the socket's ring pointers on OPEN; do the same so a reused socket
       * starts with Sn_RX_RSR=0 (see u2_reset_socket_rings — fixes the Contiki RECV storm). */
      u2_reset_socket_rings(i);
      __atomic_store_n(&u2_sn_ir[i], 0, __ATOMIC_RELEASE);
      uint8_t proto = mr & W5100_SN_MR_PROTO_MASK;
      if (proto == W5100_SN_MR_MACRAW) {
        int ok = (U2_Net_OpenMacraw(i) == 0);
        U2_MonSockOpen(i, mr, port, ok);
        if (ok) {
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_SOCK_MACRAW;
#if UTHERNET2_DEBUG
          U2_MonCheckpoint(5);
#endif
        } else
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      } else if (proto == W5100_SN_MR_TCP || proto == W5100_SN_MR_UDP || proto == W5100_SN_MR_IPRAW) {
        u2_defer_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = U2_DF_OPEN;
        cmd.mr = mr;
        cmd.port = port;
        cmd.proto = u2_memory[(address & 0xFF00) + W5100_SN_PROTO];
        u2_defer_arm(i, &cmd);
        hold_cr = 1;
      } else {
        U2_MonSockOpen(i, mr, port, 0);
        u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      }
      break;
    }
    case W5100_SN_CR_CONNECT: {
      u2_defer_t cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.op = U2_DF_CONNECT;
      cmd.dip = (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR0] << 24
              | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR1] << 16
              | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR2] << 8
              | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR3];
      cmd.dport = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_DPORT0] << 8
                | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_DPORT1];
      cmd.port = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
               | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
      u2_defer_arm(i, &cmd);
      hold_cr = 1;
      break;
    }
    case W5100_SN_CR_LISTEN: {
      u2_defer_t cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.op = U2_DF_LISTEN;
      cmd.port = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
               | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
      u2_defer_arm(i, &cmd);
      hold_cr = 1;
      break;
    }
    case W5100_SN_CR_CLOSE:
    case W5100_SN_CR_DISCON:
      if (U2_Net_GetStatus(i) == W5100_SN_SR_SOCK_MACRAW) {
        U2_MonSockClose(i);
        U2_Net_Close(i);
        u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      } else {
        u2_defer_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = (value == W5100_SN_CR_DISCON) ? U2_DF_DISCON : U2_DF_CLOSE;
        u2_defer_arm(i, &cmd);
        hold_cr = 1;
      }
      break;
    case W5100_SN_CR_SEND:
      U2_MonSockSendRecv(i, 1);
      if (U2_Net_GetStatus(i) == W5100_SN_SR_SOCK_MACRAW) {
        if (send_data(i) != 0) {
          U2_RequestCore0NetPoll();
          return;
        }
      } else {
        u2_defer_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.op = U2_DF_SEND;
        cmd.tx_rd = read_net16(&u2_memory[(address & 0xFF00) + W5100_SN_TX_RD0]);
        cmd.tx_wr = read_net16(&u2_memory[(address & 0xFF00) + W5100_SN_TX_WR0]);
        cmd.dip = (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR0] << 24
                | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR1] << 16
                | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR2] << 8
                | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR3];
        cmd.dport = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_DPORT0] << 8
                  | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_DPORT1];
        u2_defer_arm(i, &cmd);
        hold_cr = 1;
      }
      U2_RequestCore0NetPoll();
      break;
    case W5100_SN_CR_RECV: {
#if UTHERNET2_DEBUG
      if (!u2_dbg_stall_dumped[i])
#endif
      U2_MonSockSendRecv(i, 0);
      /* W5100 semantics: host updates RX_RD to consumed length before RECV.
       * Do not force RX_RD->WR here; that drops unread tail data and breaks
       * shared-access partial reads (notably wget65). */
      /* Publish the host's committed Sn_RX_RD to the core-0 shadow HERE — the authoritative,
       * driver-order-independent sync point (§1ch). Both RX_RD bytes are final at RECV, so
       * core 0's free-space math and Sn_RX_RSR never see a torn/boundary-crossed pointer
       * whether the driver wrote hi-then-lo (ip65) or lo-then-hi (Contiki). The published
       * value is always a real committed RD (never ahead of reality → never overwrites
       * unread bytes; never staled-low across a boundary → RSR converges to 0, no storm). */
      {
        uint16_t ra = u2_sockets[i].register_address;
        uint16_t rd = (uint16_t)(((uint16_t)u2_memory[ra + W5100_SN_RX_RD0] << 8)
                                 | u2_memory[ra + W5100_SN_RX_RD1]);
#if U2_RX_AUDIT
        /* §1dh: compare what the host says it consumed against what we actually served.
         * Done here, before the shadow is republished, because sn_rx_rd still holds the
         * previous committed RD. */
        if (i == 0 && u2_sockets[0].receive_size) {
          uint16_t m = (uint16_t)(u2_sockets[0].receive_size - 1u);
          uint16_t prev_rd = u2_rx_rd_load(&u2_sockets[0]);
          uint16_t advance = (uint16_t)((rd - prev_rd) & m);
          uint32_t seen = u2_audit_reads;
          uint16_t off0 = (uint16_t)(prev_rd & m);
          u2_audit_reads = 0;
          g_u2_audit_frames++;
          /* H8 control: count every wrapping frame, not just the mismatching ones. */
          if ((uint16_t)(off0 + advance) > m)
            g_u2_audit_wrap_total++;
          if (seen == advance) {
            g_u2_audit_ok++;
          } else {
            uint8_t kind;
            g_u2_audit_last_delta = (int32_t)seen - (int32_t)advance;
            if (seen == 0) {
              kind = 3;
              g_u2_audit_skip++;          /* frame discarded unread: legitimate, not a drop */
            } else if (seen < advance) {
              uint32_t deficit = advance - seen;
              kind = 1;
              g_u2_audit_short++;
              g_u2_audit_lost_bytes += deficit;
              g_u2_audit_deficit[deficit <= 8u ? deficit : 0u]++;
            } else {
              kind = 2;
              g_u2_audit_over++;
            }
            /* H6: did the bytes just consumed run off the end of the ring? If mismatches cluster
             * here, the fault is in the boundary-split re-point, not in delivery generally. */
            uint16_t off = (uint16_t)(prev_rd & m);
            uint8_t wrapped = (uint16_t)(off + advance) > m;
            if (wrapped)
              g_u2_audit_wrapped++;
            uint32_t w = u2_audit_evt_w;
            if (w - u2_audit_evt_r < U2_AUDIT_EVT_MAX) {
              u2_audit_evt_t *e = &u2_audit_evt[w % U2_AUDIT_EVT_MAX];
              uint16_t rb = u2_sockets[0].receive_base;
              e->prev_rd = prev_rd;
              e->new_rd = rd;
              e->advance = advance;
              e->seen = (uint16_t)seen;
              e->rsr = u2_rx_used_bytes_live(0);
              e->hdr = (uint16_t)(((uint16_t)u2_memory[rb + off] << 8)
                                  | u2_memory[rb + ((off + 1u) & m)]);
              e->kind = kind;
              e->wrapped = wrapped;
              __atomic_store_n(&u2_audit_evt_w, w + 1u, __ATOMIC_RELEASE);
            }
          }
        }
#endif
        {
          /* TCP window opens when the host consumes, not when the byte is copied in. */
          uint16_t prev = u2_rx_rd_load(&u2_sockets[i]);
          uint16_t sz = u2_sockets[i].receive_size;
          if (sz) {
            uint16_t adv = (uint16_t)((rd - prev) & (uint16_t)(sz - 1u));
            if (adv)
              U2_Net_NoteRecv(i, adv);
          }
        }
        __atomic_store_n(&u2_sockets[i].sn_rx_rd, rd, __ATOMIC_RELEASE);
        /* MACRAW RX wedge detect + recovery (§1ck). A single-chip W5100 can never present a frame
         * whose 2-byte length header is 0x0000 (min header = frame_len + 2 ≥ 16) or larger than
         * Sn_RX_RSR. In our dual-core model the host's Sn_RX_RD can very occasionally land off a
         * frame boundary under bulk RX (large frames wrapping the 4 KiB ring): it then reads a bogus
         * length, advances Sn_RX_RD by the wrong amount, and eventually parks on interior zero
         * padding → length 0 → Sn_RX_RD frozen → the host spins RECV forever ("sock0 RECV" storm,
         * only cured by an app restart). Because such a header is IMPOSSIBLE on real hardware, we
         * treat a persistent frozen-rd + impossible-header as a desync and restore the single-chip
         * invariant: resync Sn_RX_RD to the producer write pointer (discard the unreachable tail).
         * Sn_RX_RSR then collapses to 0, the next frame lands on a clean boundary, and the host
         * recovers on its own (its TCP retransmits any dropped payload). Guarded on an impossible
         * header + a few consecutive stalled RECVs so it never fires during normal draining. */
        {
          /* §1ck relied on Sn_RX_RD being *exactly* frozen to detect the wedge. Under bulk RX the
           * host's Sn_RX_RD instead *creeps* — it reads a bogus length, advances by the wrong (often
           * small) amount, and parks on interior bytes rather than a frame boundary. So rd is never
           * equal on two consecutive RECVs and the old freeze test never fired, letting the storm run
           * until the 6502 crashed (§1cl). Key the detector on the *header* instead: a real single-
           * chip W5100 always presents a length = frame_len + 2 (≥ 62 for a padded Ethernet frame, and
           * always ≤ Sn_RX_RSR). A header outside that range means Sn_RX_RD is off a frame boundary,
           * whether it froze or crept. After a few consecutive impossible-header RECVs we restore the
           * single-chip invariant by resyncing Sn_RX_RD to the producer write pointer; Sn_RX_RSR then
           * collapses to 0, the next frame lands clean, and the host recovers (TCP retransmits the
           * discarded tail). Genuine draining always reads at a boundary, so bad stays 0. */
          static uint16_t bad[W5100_NUM_SOCKETS];
          uint16_t sz = u2_sockets[i].receive_size;
          if (sz) {
            uint16_t m = sz - 1;
            uint16_t rsr = u2_rx_used_bytes_live(i);
            uint16_t off = (uint16_t)(rd & m);
            uint16_t rb = u2_sockets[i].receive_base;
            uint8_t h0 = u2_memory[rb + off];
            uint8_t h1 = u2_memory[rb + ((off + 1u) & m)];
            uint16_t framesize = (uint16_t)(((uint16_t)h0 << 8) | h1);
            int impossible = (rsr > 0) && (framesize < 16u || framesize > rsr);
            if (impossible) {
              bad[i]++;
#if UTHERNET2_DEBUG
              if (bad[i] == 1u && !u2_dbg_stall_dumped[i]) {
                uint16_t wr_now = u2_rx_wr_load(&u2_sockets[i]);
                uint16_t last_wire = u2_dbg_last_wire[i];
                uint16_t rec_off = (uint16_t)((wr_now - last_wire) & m);
                uint8_t rh0 = u2_memory[rb + rec_off];
                uint8_t rh1 = u2_memory[rb + ((rec_off + 1u) & m)];
                uint16_t hdr_at = (uint16_t)(((uint16_t)rh0 << 8) | rh1);
                uint8_t match = (last_wire != 0 && hdr_at == last_wire) ? 1u : 0u;
                uint32_t ring4a = 0, ring4b = 0;
                unsigned k;
                for (k = 0; k < 4u; k++)
                  ring4a = (ring4a << 8) | u2_memory[rb + ((off + k) & m)];
                for (k = 4u; k < 8u; k++)
                  ring4b = (ring4b << 8) | u2_memory[rb + ((off + k) & m)];
                U2_MonRecvStallDbg(i, last_wire, rec_off, hdr_at, match, ring4a, ring4b);
                U2_MonRecvStall(i, rsr, rd, off, h0, h1);
                u2_dbg_stall_dumped[i] = 1;
              }
#endif
              if (bad[i] >= 3u) {
                /* Log only. Do not force Sn_RX_RD=wr (§1cq). */
                uint16_t wr = u2_rx_wr_load(&u2_sockets[i]);
#if UTHERNET2_DEBUG
                if (u2_dbg_stall_dumped[i] == 1u) {
                  U2_MonRecvResync(i, rsr, rd, wr, h0, h1);
                  u2_dbg_stall_dumped[i] = 2;
                }
#else
                U2_MonRecvResync(i, rsr, rd, wr, h0, h1);
#endif
                bad[i] = 0;
              }
            } else {
              bad[i] = 0;
            }
          }
        }
      }
      U2_Net_RecvConfirm(i);
      U2_RequestCore0NetPoll();
      break;
    }
    default:
      break;
    }
    /* Command complete: clear CR so host (e.g. ip65) sees command done.
     * Deferred TCP/UDP/IPRAW commands clear CR on core 0 when the command is accepted. */
    if (!hold_cr)
      u2_memory[address] = 0;
  }
}

static void U2_BUS_RAM(write_value_at)(uint16_t address, uint8_t value) {
  if (address >= W5100_MR && address <= W5100_UPORT1) {
    write_common_register(address, value);
    return;
  }
  if (address >= W5100_S0_BASE && address <= W5100_S3_MAX) {
    write_socket_register(address, value);
    return;
  }
  if (address >= W5100_TX_BASE && address <= W5100_MEM_MAX)
    u2_memory[address] = value;
}

static void U2_BUS_RAM(write_value)(uint8_t value) {
  uint16_t wr_addr = u2_data_address;
  write_value_at(wr_addr, value);
#if UTHERNET2_DEBUG && U2_IP65_TRACE_DATA
  /* Shares the 48-op budget armed on MR=0x03 so a capture shows the driver's detection
   * writes (invisible before) interleaved with the DATA reads. */
  if (u2_ip65_data_trace_left > 0) {
    U2_MonDataWriteTrace(wr_addr, value, u2_mode_register);
    u2_ip65_data_trace_left--;
  }
#endif
  auto_increment();
}

void U2_SetStationMacFromBytes(const uint8_t mac[6]) {
  if (!mac)
    return;
  for (int i = 0; i < 6; i++)
    u2_memory[W5100_SHAR0 + i] = mac[i];
}

#if U2_RX_AUDIT
/* #region agent log
 * NDJSON over UART, one object per line, so a saved serial capture *is* the debug log file.
 * Core 0 only — printf() on the bus path is what we are trying to measure. */
#define U2_DBG_LOG(hyp_, loc_, msg_, fmt_, ...)                                                    \
  printf("{\"sessionId\":\"a36369\",\"runId\":\"run1\",\"hypothesisId\":\"" hyp_ "\","             \
         "\"location\":\"" loc_ "\",\"message\":\"" msg_ "\",\"timestamp\":%llu,\"data\":{" fmt_    \
         "}}\n",                                                                                   \
         (unsigned long long)(time_us_64() / 1000u), __VA_ARGS__)

#endif


#if U2_RX_AUDIT
void U2_RxAuditReport(void) {
  /* H1/H2: SHORT>0 proves dropped bus cycles. SHORT==0 while checksums still fail proves every
   * byte was *counted* correctly, moving the fault to the byte values (PIO/prefetch delivery). */
  U2_DBG_LOG("H1", "uthernet2.c:U2_RxAuditReport", "rx delivery audit",
             "\"frames\":%lu,\"ok\":%lu,\"short\":%lu,\"over\":%lu,\"skip\":%lu,\"lost\":%lu,"
             "\"last_delta\":%ld,\"wrapped\":%lu,\"def1\":%lu,\"def2\":%lu,\"def3\":%lu,"
             "\"def4\":%lu,\"def5plus\":%lu,\"rx_base\":%u,\"rx_size\":%u",
             (unsigned long)g_u2_audit_frames, (unsigned long)g_u2_audit_ok,
             (unsigned long)g_u2_audit_short, (unsigned long)g_u2_audit_over,
             (unsigned long)g_u2_audit_skip, (unsigned long)g_u2_audit_lost_bytes,
             (long)g_u2_audit_last_delta, (unsigned long)g_u2_audit_wrapped,
             (unsigned long)g_u2_audit_deficit[1], (unsigned long)g_u2_audit_deficit[2],
             (unsigned long)g_u2_audit_deficit[3], (unsigned long)g_u2_audit_deficit[4],
             (unsigned long)(g_u2_audit_deficit[5] + g_u2_audit_deficit[6] +
                             g_u2_audit_deficit[7] + g_u2_audit_deficit[8] +
                             g_u2_audit_deficit[0]),
             (unsigned)u2_sockets[0].receive_base, (unsigned)u2_sockets[0].receive_size);

  /* H7 vs H9: did the address walk off the ring end, and did the host notice and re-point?
   * wrap_total is the control — short/wrap_total shows whether the fault is structural. */
  U2_DBG_LOG("H7", "uthernet2.c:auto_increment", "ring-end crossings",
             "\"hit_end\":%lu,\"repoint\":%lu,\"wrap_total\":%lu,\"short\":%lu,\"ring_end\":%u",
             (unsigned long)g_u2_aud_hit_end, (unsigned long)g_u2_aud_repoint,
             (unsigned long)g_u2_audit_wrap_total, (unsigned long)g_u2_audit_short,
             (unsigned)u2_aud_ring_end);

  /* §1dl pointer trace. Emitted once, only after it has frozen around a real ring-end crossing,
   * as one entry per line so a long dump cannot stall core 0 in a single printf. Each entry is
   * "off" = pointer as a ring offset (negative/large means outside socket 0's ring) and "reads" =
   * cumulative host DATA reads at that instant; consecutive reads deltas give the burst length. */
  if (u2_trc_done && !u2_trc_dumped) {
    u2_trc_dumped = 1;
    uint32_t total = u2_trc_w < U2_TRC_MAX ? u2_trc_w : U2_TRC_MAX;
    uint32_t start = u2_trc_w - total;
    uint16_t base = u2_sockets[0].receive_base;
    for (uint32_t k = 0; k < total; k++) {
      uint32_t e = u2_trc[(start + k) & (U2_TRC_MAX - 1u)];
      uint16_t a = (uint16_t)(e & 0xFFFFu);
      uint16_t rd = u2_trc_rd[(start + k) & (U2_TRC_MAX - 1u)];
      uint16_t msk = (uint16_t)(u2_sockets[0].receive_size - 1u);
      /* rd_off is the host-visible Sn_RX_RD as a ring offset; skew is pointer minus rd_off, the
       * §1dn decider. Consistent skew of 2 on the pointer the host writes means our RD trails the
       * record start; skew 0 means the driver's own split arithmetic is 2 late. */
      U2_DBG_LOG("H10", "uthernet2.c:U2_HandleBusAccess", "pointer trace",
                 "\"i\":%lu,\"reg\":\"%s\",\"addr\":%u,\"off\":%d,\"reads\":%u,"
                 "\"rd\":%u,\"rd_off\":%u,\"skew\":%d,\"rsr\":%u,\"limit\":%d,\"ring_end\":%u",
                 (unsigned long)k, (e & U2_TRC_HIGH) ? "hi" : "lo", (unsigned)a,
                 (int)a - (int)base, (unsigned)(e >> 17), (unsigned)rd, (unsigned)(rd & msk),
                 (int)(a - base) - (int)(rd & msk),
                 (unsigned)u2_trc_rsr[(start + k) & (U2_TRC_MAX - 1u)],
                 (int)u2_sockets[0].receive_size - (int)(rd & msk), (unsigned)u2_aud_ring_end);
    }
  }

  /* H4: is core 0 being starved (blocking UART writes) long enough to stall Contiki? */
  U2_DBG_LOG("H4", "uthernet2.c:U2_RxAuditReport", "core0 service gaps",
             "\"polls\":%lu,\"gap_max_us\":%lu,\"gap_gt5ms\":%lu,\"gap_gt20ms\":%lu,"
             "\"gap_gt100ms\":%lu",
             (unsigned long)g_u2_core0_polls, (unsigned long)g_u2_core0_gap_max_us,
             (unsigned long)g_u2_core0_gap_5ms, (unsigned long)g_u2_core0_gap_20ms,
             (unsigned long)g_u2_core0_gap_100ms);

  /* H1/H6 detail: where each mismatch happened, and whether it sat on the ring wrap. */
  uint32_t r = u2_audit_evt_r;
  uint32_t w = __atomic_load_n(&u2_audit_evt_w, __ATOMIC_ACQUIRE);
  if (w - r > 6u)
    w = r + 6u; /* cap per report: every line here is core-0 time spent blocked on the UART */
  while (r != w) {
    const u2_audit_evt_t *e = &u2_audit_evt[r % U2_AUDIT_EVT_MAX];
    U2_DBG_LOG("H6", "uthernet2.c:RECV", "rx mismatch detail",
               "\"kind\":%u,\"prev_rd\":%u,\"new_rd\":%u,\"advance\":%u,\"seen\":%u,"
               "\"deficit\":%d,\"wrapped\":%u,\"rsr\":%u,\"hdr\":%u",
               (unsigned)e->kind, (unsigned)e->prev_rd, (unsigned)e->new_rd,
               (unsigned)e->advance, (unsigned)e->seen,
               (int)e->seen - (int)e->advance, (unsigned)e->wrapped,
               (unsigned)e->rsr, (unsigned)e->hdr);
    r++;
  }
  u2_audit_evt_r = r;
}
/* #endregion */
#endif

static volatile uint8_t u2_stage_b[16];
static volatile uint32_t u2_stage_us[16];
static volatile uint8_t u2_stage_w;
static volatile uint8_t u2_stage_r;

void U2_BUS_RAM(U2_NoteStage)(uint8_t id) {
  uint8_t w = u2_stage_w;
  uint8_t nxt = (uint8_t)((w + 1u) & 15u);
  if (nxt == u2_stage_r)
    return;
  u2_stage_b[w] = id;
  u2_stage_us[w] = time_us_32();
  u2_stage_w = nxt;
}

void U2_StagePoll(void) {
  uint8_t r = u2_stage_r;
  uint8_t w = u2_stage_w;
  while (r != w) {
    // #region agent log
    U2_AgentLog("H31", "stg", (int)u2_stage_b[r], (int)u2_stage_us[r], 0, 1);
    // #endregion
    r = (uint8_t)((r + 1u) & 15u);
    u2_stage_r = r;
  }
}

void U2_AgentLog(const char *hid, const char *msg, int a, int b, int c, int force) {
  // #region agent log
  static uint16_t u2_agent_n;
  if (u2_agent_n >= 400)
    return;
  if (!force) {
    if ((u2_agent_n & 15u) != 0) {
      u2_agent_n++;
      return;
    }
  }
  u2_agent_n++;
  printf("{\"sessionId\":\"98f944\",\"hypothesisId\":\"%s\",\"location\":\"uthernet2\",\"message\":\"%s\",\"data\":{\"a\":%d,\"b\":%d,\"c\":%d},\"timestamp\":%llu}\n",
         hid, msg, a, b, c, (unsigned long long)(time_us_64() / 1000u));
  // #endregion
}

void U2_Init(void) {
  /* Socket-mode probe: Release turns UART off. Bring it back here so a few
   * NDJSON lines are visible on GPIO 0/1 without the Debug monitor flood. */
  stdio_uart_init();
  stdio_set_driver_enabled(&stdio_uart, true);
  setbuf(stdout, NULL);
#if U2_RX_AUDIT
  /* The audit must run in a Release build to keep the bus path's normal timing, but Release
   * disables UART stdio (main.c) leaving only USB CDC — and a connected USB console gates the
   * bus loop off entirely (§1da), so USB cannot be the sink here. U2_Init runs immediately after
   * main.c's stdio setup, so bring the UART back up from here and leave main.c alone. */
  stdio_uart_init();
  stdio_set_driver_enabled(&stdio_uart, true);
  setbuf(stdout, NULL);
#endif
  U2_DEBUGF("init\n");
  U2_MonInit();
#if UTHERNET2_DEBUG
  printf("[u2] ip65 debug: U2_IP65_CHECKPOINT=%d (0=off; 1..5=bisect; cmake -DU2_IP65_CHECKPOINT=n)\n",
         (int)U2_IP65_CHECKPOINT);
#endif
  u2_data_address = 0;
  U2_Net_Init(u2_push_rx, u2_push_rx_macraw);
  u2_reset();
  // #region agent log
  U2_AgentLog("H5", "boot", 0, 0, 0, 1);
  // #endregion
}

#if PICO_CYW43_ARCH_POLL
/* Set on core 1 from the SEND/RECV handlers, cleared on core 0 in U2_Net_Poll.
 * §1cx: this used to read the APB timer and push an IPC message through the multicore FIFO
 * (multicore_fifo_push_timeout_us itself calls make_timeout_time_us + time_reached), all from
 * XIP flash contended by core 0's lwIP. That ran on the bus path, where the budget before the
 * 6502's next $C0C7 read is ~90 ns. Core 0 polls the network unconditionally with a zero FIFO
 * timeout (PicoW_ServiceCore0IpcAndNetwork), so there is nothing to unblock — a single store to
 * an SRAM flag conveys the same request at no cost to core 1. */
void U2_BUS_RAM(U2_RequestCore0NetPoll)(void) { u2_core0_net_wake_pending = true; }
#else
void U2_RequestCore0NetPoll(void) {}
#endif

volatile bool u2_core0_net_wake_pending;

void U2_BUS_RAM(U2_HandleBusAccess)(uint32_t busdata, uint8_t *read_byte_out) {
  u2_bus_n++;
  uint32_t loc = busdata & U2_C0X_MASK;
  uint8_t data = (uint8_t)((busdata >> 5) & 0xFF);
  int is_read = (busdata & READFLAG) != 0;
  uint16_t adr_before = u2_data_address;

  *read_byte_out = 0;
  if (is_read) {
    uint8_t res;
    switch (loc) {
    case U2_C0X_MODE_REGISTER:
      res = u2_mode_register;
      break;
    case U2_C0X_ADDRESS_HIGH:
      res = get_byte(u2_data_address, 8);
      break;
    case U2_C0X_ADDRESS_LOW:
      res = get_byte(u2_data_address, 0);
      break;
    case U2_C0X_DATA_PORT:
      res = read_value();
      break;
    default:
      res = 0;
      break;
    }
    *read_byte_out = res;
  } else {
    switch (loc) {
    case U2_C0X_MODE_REGISTER:
      if (data & W5100_MR_RST)
        u2_reset();
      else {
        u2_mode_register = data;
#if UTHERNET2_DEBUG
        if (data == 0x03)
          U2_MonCheckpoint(1);
#endif
#if UTHERNET2_DEBUG && U2_IP65_TRACE_DATA
        u2_arm_ip65_data_trace(data);
        U2_MonQueueModeLine(data);
#endif
      }
      break;
    case U2_C0X_ADDRESS_HIGH:
#if U2_RX_AUDIT
      /* #region agent log — H7/H9: re-pointing while already past the ring end means the host
       * noticed and will discard whatever it read there. Write path, so off the read hot path. */
      if (u2_aud_ring_end && u2_data_address >= u2_aud_ring_end &&
          u2_data_address < (uint16_t)(u2_aud_ring_end + 0x1000u))
        g_u2_aud_repoint++;
      /* #endregion */
#endif
      u2_data_address = (uint16_t)((data << 8) | (u2_data_address & 0x00FF));
#if U2_RX_AUDIT
      /* #region agent log — pointer trace, see note at u2_trc. */
      u2_trc_note(U2_TRC_HIGH);
      /* #endregion */
#endif
      break;
    case U2_C0X_ADDRESS_LOW:
      u2_data_address = (uint16_t)((data << 0) | (u2_data_address & 0xFF00));
      if (__atomic_load_n(&u2_aud_ready, __ATOMIC_ACQUIRE) &&
          u2_data_address >= W5100_RX_BASE) {
        u2_aim = u2_data_address;
        u2_aim_seen = 1;
      }
#if U2_RX_AUDIT
      /* #region agent log — pointer trace, see note at u2_trc. */
      u2_trc_note(0);
      /* #endregion */
#endif
      break;
    case U2_C0X_DATA_PORT:
      write_value(data);
      break;
    default:
      break;
    }
  }
#if UTHERNET2_DEBUG && U2_MON_LOG_BUS
  {
    uint8_t log_byte = is_read ? *read_byte_out : data;
    U2_MonBus(is_read, (unsigned)loc, busdata, log_byte, u2_data_address, u2_mode_register);
  }
#endif
  if (__atomic_load_n(&u2_aud_ready, __ATOMIC_ACQUIRE)) {
    uint8_t i = u2_cyc_i;
    struct u2_cyc_s *e = &u2_cyc[i & 7u];
    e->adr = is_read ? adr_before : u2_data_address;
    e->loc = (uint8_t)loc;
    e->rw = (uint8_t)(is_read ? 1 : 0);
    e->val = is_read ? *read_byte_out : data;
    e->pk = 0;
    u2_cyc_i = (uint8_t)(i + 1u);
  }
}

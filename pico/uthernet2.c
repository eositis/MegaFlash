/**
 * Uthernet II (W5100) emulation at C0x4–C0x7 (slot 4: $C0C4–$C0C7).
 * W5100 register and memory state; C0x interface; network stack and RX path.
 */
#include "uthernet2.h"
#include "uthernet2_net.h"
#include "u2_monitor.h"
#include "w5100_regs.h"
#include "ipc.h"
#include "pico.h"
#include "pico/multicore.h"
#include "pico/time.h"
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
}

static void u2_reset(void) {
#if UTHERNET2_DEBUG && U2_IP65_TRACE_DATA
  u2_ip65_data_trace_left = 0;
#endif
  U2_MonReset();
  memset(u2_memory, 0, sizeof(u2_memory));
  u2_mode_register = 0;
  for (int i = 0; i < W5100_NUM_SOCKETS; i++)
    U2_Net_Close(i);
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
  if (address >= W5100_GAR0 && address <= W5100_UPORT1)
    return u2_memory[address];
  if (address >= W5100_S0_BASE && address <= W5100_S3_MAX)
    return read_socket_register(address);
  if (address >= W5100_TX_BASE && address <= W5100_MEM_MAX)
    return u2_memory[address];
  return u2_memory[address & W5100_MEM_MAX];
}

uint8_t U2_BUS_RAM(U2_PeekDataPort)(void) {
  /* Pair with core-0 release-store of sn_rx_wr so DATA peeks see the frame bytes. */
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return read_value_at(u2_data_address);
}

static void U2_BUS_RAM(auto_increment)(void) {
  if (u2_mode_register & W5100_MR_AI) {
    u2_data_address++;
    if (u2_data_address == W5100_RX_BASE || u2_data_address == W5100_MEM_SIZE)
      u2_data_address -= 0x2000;
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
static volatile uint32_t u2_audit_reads;  /* in-window DATA reads since last RECV */

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

/* H7: the 2 bytes the audit found missing at every wrap. If the host's DATA reads run off the end
 * of socket 0's ring, they land in 0x7000+ (socket 1's region, never written by the producer) and
 * the frame is delivered with 2 foreign bytes in it. Record the actual addresses so the claim is
 * localized rather than inferred. */
volatile uint32_t g_u2_oow_reads;
#define U2_OOW_MAX 16u
static uint16_t u2_oow_addr[U2_OOW_MAX];
static volatile uint32_t u2_oow_w, u2_oow_r;

/* H8 control: how many frames crossed the ring end in total, matched or not. If every wrapping
 * frame is short, the fault is structural; if only some are, it is a race. */
volatile uint32_t g_u2_audit_wrap_total;

/* Count only real host $C0C7 reads that land inside socket 0's RX ring. The prefetch path
 * (U2_PeekDataPort -> read_value_at) deliberately bypasses this, so peeks are never counted.
 * Plain `static inline` (no U2_BUS_RAM): it inlines into read_value, which is already
 * RAM-resident, so pinning it separately would only force a noinline call on the hot path. */
static inline void u2_audit_note_read(uint16_t addr) {
  const u2_socket_t *s = &u2_sockets[0];
  uint16_t sz = s->receive_size;
  if (!sz)
    return;
  if ((uint16_t)(addr - s->receive_base) < sz) {
    u2_audit_reads++;
  } else if (addr >= W5100_RX_BASE) {
    /* H7: a DATA read inside the RX memory region but outside socket 0's ring — i.e. past the
     * ring end, where the producer never writes. Capture the address itself. */
    g_u2_oow_reads++;
    uint32_t w = u2_oow_w;
    if (w - u2_oow_r < U2_OOW_MAX) {
      u2_oow_addr[w % U2_OOW_MAX] = addr;
      __atomic_store_n(&u2_oow_w, w + 1u, __ATOMIC_RELEASE);
    }
  }
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
      (address >= W5100_SIPR0 && address <= W5100_SIPR3))
    u2_memory[address] = value;
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
  if (is_udp) {
    total += 8;  /* 4 + 2 + 2 */
    if (free_bytes <= total) {
      used = u2_rx_used_bytes(socket_i);
      free_bytes = size - used;
      if (free_bytes <= total) {
        U2_MonNetRxDrop(socket_i, U2_RX_PROTO_UDP, U2_RX_DROP_NO_ROOM, len, 0, free_bytes, size);
        return 0;  /* UDP datagram is atomic */
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
  if (is_udp) {
    u2_memory[base + (wr & mask)] = (uint8_t)(src_ip >> 24);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)(src_ip >> 16);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)(src_ip >> 8);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)src_ip;
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)(src_port >> 8);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)src_port;
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)(len >> 8);
    wr++;
    u2_memory[base + (wr & mask)] = (uint8_t)len;
    wr++;
  }
  for (uint16_t k = 0; k < accept_len; k++) {
    u2_memory[base + (wr & mask)] = data[k];
    wr++;
  }
  u2_rx_wr_store(s, wr);
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
    if (!is_broadcast && !is_ours)
      return;
  }
  uint16_t size = s->receive_size;
  if (size == 0) return;
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
  u2_memory[base + (wr & mask)] = (uint8_t)(wire_len >> 8);
  wr++;
  u2_memory[base + (wr & mask)] = (uint8_t)wire_len;
  wr++;
  for (uint16_t k = 0; k < len; k++) {
    u2_memory[base + (wr & mask)] = data[k];
    wr++;
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
      if (U2_Net_SendMacraw(i, buf, (uint16_t)n) != 0)
        return -1; /* not accepted: do NOT advance TX_RD, retry on next SEND */
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
static void U2_BUS_RAM(write_socket_register)(uint16_t address, uint8_t value) {
  u2_memory[address] = value;
  uint16_t loc = address & 0xFF;
  /* NOTE: Sn_RX_RD byte writes deliberately do NOT publish the core-0 shadow. On a real W5100
   * the host's Sn_RX_RD update only takes effect (Sn_RX_RSR is recomputed) when the RECV command
   * is issued; the shadow is therefore published in the RECV handler below, where both RX_RD bytes
   * are final. Publishing on the low-byte write (§1cf) assumed the driver writes RX_RD hi-then-lo
   * (ip65); Contiki writes lo-then-hi, so publishing on the low byte captured {old_hi:new_lo} and
   * never re-published the high byte — across a 256-byte boundary the shadow lagged by 256 and
   * over-reported Sn_RX_RSR, resurrecting the unbounded "sock0 RECV" storm (§1ch). */
  if (loc == W5100_SN_CR) {
    int i = (address >> 8) - 0x04;
    switch (value) {
    case W5100_SN_CR_OPEN: {
      uint8_t mr = u2_memory[(address & 0xFF00) + W5100_SN_MR];
      uint16_t port = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
                    | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
      /* Hardware resets the socket's ring pointers on OPEN; do the same so a reused socket
       * starts with Sn_RX_RSR=0 (see u2_reset_socket_rings — fixes the Contiki RECV storm). */
      u2_reset_socket_rings(i);
      switch (mr & W5100_SN_MR_PROTO_MASK) {
      case W5100_SN_MR_UDP: {
        int ok = (U2_Net_OpenUdp(i, port) == 0);
        U2_MonSockOpen(i, mr, port, ok);
        if (!ok)
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
        break;
      }
      case W5100_SN_MR_TCP: {
        int ok = (U2_Net_OpenTcp(i) == 0);
        U2_MonSockOpen(i, mr, port, ok);
        if (ok)
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_SOCK_INIT;
        else
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
        break;
      }
      case W5100_SN_MR_MACRAW: {
        int ok = (U2_Net_OpenMacraw(i) == 0);
        U2_MonSockOpen(i, mr, port, ok);
        if (ok) {
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_SOCK_MACRAW;
#if UTHERNET2_DEBUG
          U2_MonCheckpoint(5);
#endif
        } else
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
        break;
      }
      default:
        U2_MonSockOpen(i, mr, port, 0);
        u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
        break;
      }
      break;
    }
    case W5100_SN_CR_CONNECT: {
      uint32_t dip = (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR0] << 24
                   | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR1] << 16
                   | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR2] << 8
                   | (uint32_t)u2_memory[(address & 0xFF00) + W5100_SN_DIPR3];
      uint16_t dport = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_DPORT0] << 8
                     | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_DPORT1];
      uint16_t lport = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
                     | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
      {
        int cok = (U2_Net_ConnectTcpEx(i, dip, dport, lport) == 0);
        U2_MonSockConnect(i, dip, dport, cok);
        if (!cok)
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      }
      break;
    }
    case W5100_SN_CR_LISTEN: {
      uint16_t port = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
                    | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
      {
        int lok = (U2_Net_ListenTcp(i, port) == 0);
        U2_MonSockListen(i, port, lok);
        if (!lok)
          u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      }
      break;
    }
    case W5100_SN_CR_CLOSE:
    case W5100_SN_CR_DISCON:
      U2_MonSockClose(i);
      U2_Net_Close(i);
      u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      break;
    case W5100_SN_CR_SEND:
      U2_MonSockSendRecv(i, 1);
      if (send_data(i) != 0) {
        U2_RequestCore0NetPoll();
        return;
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
    /* Command complete: clear CR so host (e.g. ip65) sees command done */
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

  /* H7/H8: the addresses the host actually read past the ring end, plus the wrapping-frame
   * total so short/wrap_total shows whether the fault is structural or a race. */
  {
    uint32_t r = u2_oow_r;
    uint32_t w = __atomic_load_n(&u2_oow_w, __ATOMIC_ACQUIRE);
    if (w - r > 8u)
      w = r + 8u;
    U2_DBG_LOG("H7", "uthernet2.c:read_value", "reads past ring end",
               "\"oow_reads\":%lu,\"wrap_total\":%lu,\"short\":%lu,\"captured\":%lu",
               (unsigned long)g_u2_oow_reads, (unsigned long)g_u2_audit_wrap_total,
               (unsigned long)g_u2_audit_short, (unsigned long)(w - r));
    while (r != w) {
      U2_DBG_LOG("H7", "uthernet2.c:read_value", "oow addr",
                 "\"addr\":%u,\"addr_hex\":\"0x%04X\",\"rx_end\":%u",
                 (unsigned)u2_oow_addr[r % U2_OOW_MAX], (unsigned)u2_oow_addr[r % U2_OOW_MAX],
                 (unsigned)(u2_sockets[0].receive_base + u2_sockets[0].receive_size));
      r++;
    }
    u2_oow_r = r;
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

void U2_Init(void) {
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
  uint32_t loc = busdata & U2_C0X_MASK;
  uint8_t data = (uint8_t)((busdata >> 5) & 0xFF);
  int is_read = (busdata & READFLAG) != 0;

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
      u2_data_address = (uint16_t)((data << 8) | (u2_data_address & 0x00FF));
      break;
    case U2_C0X_ADDRESS_LOW:
      u2_data_address = (uint16_t)((data << 0) | (u2_data_address & 0xFF00));
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
}

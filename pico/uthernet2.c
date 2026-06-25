/**
 * Uthernet II (W5100) emulation at C0x4–C0x7 (slot 4: $C0C4–$C0C7).
 * W5100 register and memory state; C0x interface; network stack and RX path.
 */
#include "uthernet2.h"
#include "uthernet2_net.h"
#include "u2_monitor.h"
#include "w5100_regs.h"
#include "pico.h"
#include <stdio.h>
#include <string.h>

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
} u2_socket_t;

static u2_socket_t u2_sockets[W5100_NUM_SOCKETS];

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

static uint16_t read_net16(const uint8_t *p) {
  return (uint16_t)p[0] << 8 | p[1];
}

static inline uint16_t u2_rx_wr_load(const u2_socket_t *s) {
  return __atomic_load_n(&s->sn_rx_wr, __ATOMIC_ACQUIRE);
}

static inline void u2_rx_wr_store(u2_socket_t *s, uint16_t wr) {
  __atomic_store_n(&s->sn_rx_wr, wr, __ATOMIC_RELEASE);
}

/* Host writes Sn_RX_RD0/1 as two 8-bit stores on core 1; core 0 may read mid-update. */
static uint16_t read_rx_rd_coherent(const u2_socket_t *s) {
  const uint8_t *p = &u2_memory[s->register_address + W5100_SN_RX_RD0];
  uint8_t hi1 = p[0];
  uint8_t lo = p[1];
  uint8_t hi2 = p[0];
  if (hi1 == hi2)
    return (uint16_t)((hi1 << 8) | lo);
  return (uint16_t)((hi2 << 8) | lo);
}

/* Unread bytes in emulated RX ring. ip65 keeps Sn_RX_RD as a physical W5100 address;
 * mask extracts ring offset when receive_base is size-aligned (RMSR layout). */
static uint16_t u2_rx_used_bytes(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t size = s->receive_size;
  if (size == 0)
    return 0;
  uint16_t mask = size - 1;
  uint16_t rd_off = (uint16_t)(read_rx_rd_coherent(s) & mask);
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

static uint16_t get_tx_data_size(int i) {
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

static uint8_t get_tx_fsr_byte(int i, unsigned shift) {
  uint16_t ts = u2_sockets[i].transmit_size;
  if (ts == 0)
    return 0;
  uint16_t free_size = ts - get_tx_data_size(i);
  return get_byte((uint16_t)free_size, shift);
}

/* W5100 RX occupancy: RSR = unread bytes in ring (§10l). */
static uint16_t get_rx_rsr(int i) {
  return u2_rx_used_bytes(i);
}

static uint8_t get_rx_rsr_byte(int i, unsigned shift) {
  return get_byte(get_rx_rsr(i), shift);
}

static uint8_t read_socket_register(uint16_t address) {
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
  return read_value_at(u2_data_address);
}

static void U2_BUS_RAM(auto_increment)(void) {
  if (u2_mode_register & W5100_MR_AI) {
    u2_data_address++;
    if (u2_data_address == W5100_RX_BASE || u2_data_address == W5100_MEM_SIZE)
      u2_data_address -= 0x2000;
  }
}

static uint8_t U2_BUS_RAM(read_value)(void) {
  uint16_t rd_addr = u2_data_address;
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
    if (free_bytes < total) {
      used = u2_rx_used_bytes(socket_i);
      free_bytes = size - used;
      if (free_bytes < total) {
        U2_MonNetRxDrop(socket_i, U2_RX_PROTO_UDP, U2_RX_DROP_NO_ROOM, len, 0, free_bytes, size);
        return 0;  /* UDP datagram is atomic */
      }
    }
  } else {
    if (free_bytes == 0) {
      used = u2_rx_used_bytes(socket_i);
      free_bytes = size - used;
      if (free_bytes == 0) {
        U2_MonNetRxDrop(socket_i, U2_RX_PROTO_TCP, U2_RX_DROP_NO_ROOM, len, 0, free_bytes, size);
        return 0;
      }
    }
    if (accept_len > free_bytes) accept_len = free_bytes;
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
}

/* Push MACRAW frame: 2-byte length (big-endian) then frame data. */
static void u2_push_rx_macraw(int socket_i, const uint8_t *data, uint16_t len) {
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS || !data) return;
  u2_socket_t *s = &u2_sockets[socket_i];
  uint16_t size = s->receive_size;
  if (size == 0) return;
  uint16_t mask = size - 1;
  uint16_t base = s->receive_base;
  uint16_t total = (uint16_t)(2 + len);
  uint16_t used = u2_rx_used_bytes(socket_i);
  uint16_t free_bytes = size - used;
  if (total > size) {
    U2_MonNetRxDrop(socket_i, U2_RX_PROTO_MACRAW, U2_RX_DROP_FRAME_TOO_BIG, len, 0, free_bytes, size);
    return;
  }
  if (free_bytes < total) {
#if U2_MACRAW_COMPAT_DROP_OLDEST
    /* Compatibility: flush unread once so DHCP/ARP bursts can progress (enable via -DU2_MACRAW_COMPAT_DROP_OLDEST=1). */
    u2_socket_discard_rx(socket_i);
    used = u2_rx_used_bytes(socket_i);
    free_bytes = size - used;
#endif
    if (free_bytes < total) {
      used = u2_rx_used_bytes(socket_i);
      free_bytes = size - used;
    }
    if (free_bytes < total) {
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
}

/* Read TX buffer data between rd and wr and send via network */
static void send_data(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t buf_size = s->transmit_size;
  if (buf_size == 0) return;
  uint16_t mask = buf_size - 1;
  const uint8_t *r = &u2_memory[s->register_address];
  uint16_t rd_full = read_net16(r + W5100_SN_TX_RD0);
  uint16_t wr_full = read_net16(r + W5100_SN_TX_WR0);
  uint16_t rd = rd_full & mask;
  uint16_t wr = wr_full & mask;
  int data_len = (int)wr - (int)rd;
  if (data_len < 0) data_len += buf_size;
  if (data_len == 0) return;
  uint16_t base = s->transmit_base;
  uint8_t status = U2_Net_GetStatus(i);
  if (status == W5100_SN_SR_SOCK_UDP) {
    uint32_t dip = (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR0] << 24
                  | (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR1] << 16
                  | (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR2] << 8
                  | (uint32_t)u2_memory[s->register_address + W5100_SN_DIPR3];
    uint16_t dport = (uint16_t)u2_memory[s->register_address + W5100_SN_DPORT0] << 8
                  | (uint16_t)u2_memory[s->register_address + W5100_SN_DPORT1];
    /* Copy payload out (may wrap) */
    uint8_t buf[2048];
    int n = data_len;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    for (int j = 0; j < n; j++)
      buf[j] = u2_memory[base + ((rd + j) & mask)];
    U2_MonNetUdpSend(i, dip, dport, (uint16_t)n);
    U2_Net_SendUdp(i, buf, (uint16_t)n, dip, dport);
  } else if (status == W5100_SN_SR_ESTABLISHED) {
    /* Shared-access clients (e.g. wget65) can queue >2 KiB before SEND.
     * Send in chunks so one SEND command drains the full TX window. */
    uint8_t buf[1024];
    int remaining = data_len;
    int pos = 0;
    U2_MonNetTcpSend(i, (uint16_t)data_len);
    while (remaining > 0) {
      int n = remaining;
      if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
      for (int j = 0; j < n; j++)
        buf[j] = u2_memory[base + ((rd + pos + j) & mask)];
      U2_Net_SendTcp(i, buf, (uint16_t)n);
      pos += n;
      remaining -= n;
    }
  } else if (status == W5100_SN_SR_SOCK_MACRAW) {
    uint8_t buf[1518];
    int n = data_len;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    U2_MonNetMacrawTxPtrs(i, (uint16_t)n, rd_full, wr_full, rd, wr);
    for (int j = 0; j < n; j++)
      buf[j] = u2_memory[base + ((rd + j) & mask)];
    U2_MonNetMacrawTx(i, (uint16_t)n);
    U2_Net_SendMacraw(i, buf, (uint16_t)n);
  }
  /* Advance TX_RD to TX_WR */
  /* Keep full host-visible pointer progression; only ring indexing is masked. */
  u2_memory[s->register_address + W5100_SN_TX_RD0] = (uint8_t)(wr_full >> 8);
  u2_memory[s->register_address + W5100_SN_TX_RD1] = (uint8_t)wr_full;
}

static void write_socket_register(uint16_t address, uint8_t value) {
  u2_memory[address] = value;
  uint16_t loc = address & 0xFF;
  if (loc == W5100_SN_CR) {
    int i = (address >> 8) - 0x04;
    switch (value) {
    case W5100_SN_CR_OPEN: {
      uint8_t mr = u2_memory[(address & 0xFF00) + W5100_SN_MR];
      uint16_t port = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
                    | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
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
      send_data(i);
      break;
    case W5100_SN_CR_RECV: {
      U2_MonSockSendRecv(i, 0);
      /* W5100 semantics: host updates RX_RD to consumed length before RECV.
       * Do not force RX_RD->WR here; that drops unread tail data and breaks
       * shared-access partial reads (notably wget65). */
      U2_Net_RecvConfirm(i);
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
  write_value_at(u2_data_address, value);
  auto_increment();
}

void U2_SetStationMacFromBytes(const uint8_t mac[6]) {
  if (!mac)
    return;
  for (int i = 0; i < 6; i++)
    u2_memory[W5100_SHAR0 + i] = mac[i];
}

void U2_Init(void) {
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

void U2_Poll(void) {
  U2_Net_Poll();
  /* U2_MonPollFlush: call from core 0 only — see u2_monitor.h (stdio + cyw43 async_context). */
}

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

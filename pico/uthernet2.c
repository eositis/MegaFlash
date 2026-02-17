/**
 * Uthernet II (W5100) emulation at C0x4–C0x7 (slot 4: $C0C4–$C0C7).
 * W5100 register and memory state; C0x interface; network stack and RX path.
 */
#include "uthernet2.h"
#include "uthernet2_net.h"
#include "w5100_regs.h"
#include <string.h>

#define READFLAG  (1u << 4)

static uint8_t  u2_memory[W5100_MEM_SIZE];
static uint8_t  u2_mode_register;
static uint16_t u2_data_address;

typedef struct {
  uint16_t transmit_base;
  uint16_t transmit_size;
  uint16_t receive_base;
  uint16_t receive_size;
  uint16_t register_address;
  uint16_t sn_rx_wr;  /* next write offset in RX buffer (0..receive_size-1) */
} u2_socket_t;

static u2_socket_t u2_sockets[W5100_NUM_SOCKETS];

static inline uint8_t get_byte(uint16_t val, unsigned shift) {
  return (uint8_t)((val >> shift) & 0xFF);
}

static uint16_t read_net16(const uint8_t *p) {
  return (uint16_t)p[0] << 8 | p[1];
}

static void u2_reset(void) {
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
  u2_memory[W5100_RTR0] = 0x07;
  u2_memory[W5100_RTR1] = 0xD0;
  u2_memory[W5100_RCR]  = 0x08;
  u2_memory[W5100_PTIMER] = 0x28;
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
  /* set default buffer sizes (same as AppleWin: 0x55 = 2K per socket) */
  u2_memory[W5100_RMSR] = 0x55;
  u2_memory[W5100_TMSR] = 0x55;
  /* apply sizes to socket base/size */
  {
    uint16_t base = W5100_TX_BASE;
    const uint16_t end = W5100_RX_BASE;
    uint8_t val = 0x55;
    for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
      u2_sockets[i].transmit_base = base;
      uint16_t size = (uint16_t)(1 << (10 + (val & 3)));
      val >>= 2;
      base += size;
      if (base > end) base = end;
      u2_sockets[i].transmit_size = base - u2_sockets[i].transmit_base;
    }
  }
  {
    uint16_t base = W5100_RX_BASE;
    const uint16_t end = W5100_MEM_SIZE;
    uint8_t val = 0x55;
    for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
      u2_sockets[i].receive_base = base;
      uint16_t size = (uint16_t)(1 << (10 + (val & 3)));
      val >>= 2;
      base += size;
      if (base > end) base = end;
      u2_sockets[i].receive_size = base - u2_sockets[i].receive_base;
    }
  }
}

static void set_tx_sizes(uint16_t address, uint8_t value) {
  u2_memory[address] = value;
  uint16_t base = W5100_TX_BASE;
  const uint16_t end = W5100_RX_BASE;
  uint8_t val = value;
  for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
    u2_sockets[i].transmit_base = base;
    uint16_t size = (uint16_t)(1 << (10 + (val & 3)));
    val >>= 2;
    base += size;
    if (base > end) base = end;
    u2_sockets[i].transmit_size = base - u2_sockets[i].transmit_base;
  }
}

static void set_rx_sizes(uint16_t address, uint8_t value) {
  u2_memory[address] = value;
  uint16_t base = W5100_RX_BASE;
  const uint16_t end = W5100_MEM_SIZE;
  uint8_t val = value;
  for (int i = 0; i < W5100_NUM_SOCKETS; i++) {
    u2_sockets[i].receive_base = base;
    uint16_t size = (uint16_t)(1 << (10 + (val & 3)));
    val >>= 2;
    base += size;
    if (base > end) base = end;
    u2_sockets[i].receive_size = base - u2_sockets[i].receive_base;
  }
}

static uint16_t get_tx_data_size(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t size = s->transmit_size;
  uint16_t mask = size - 1;
  const uint8_t *r = &u2_memory[s->register_address];
  int rd = read_net16(r + W5100_SN_TX_RD0) & mask;
  int wr = read_net16(r + W5100_SN_TX_WR0) & mask;
  int data = wr - rd;
  if (data < 0) data += size;
  return (uint16_t)data;
}

static uint8_t get_tx_fsr_byte(int i, unsigned shift) {
  uint16_t free_size = u2_sockets[i].transmit_size - get_tx_data_size(i);
  return get_byte((uint16_t)free_size, shift);
}

/* RSR = bytes available = (sn_rx_wr - RX_RD + size) % size */
static uint16_t get_rx_rsr(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t size = s->receive_size;
  if (size == 0) return 0;
  uint16_t mask = size - 1;
  uint16_t rd = read_net16(&u2_memory[s->register_address + W5100_SN_RX_RD0]) & mask;
  uint16_t wr = s->sn_rx_wr & mask;
  int d = (int)wr - (int)rd;
  if (d < 0) d += size;
  return (uint16_t)d;
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
  case W5100_SN_TX_RD1:
  case W5100_SN_TX_WR0:
  case W5100_SN_TX_WR1:
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

static uint8_t read_value_at(uint16_t address) {
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

static void auto_increment(void) {
  if (u2_mode_register & W5100_MR_AI) {
    u2_data_address++;
    if (u2_data_address == W5100_RX_BASE || u2_data_address == W5100_MEM_SIZE)
      u2_data_address -= 0x2000;
  }
}

static uint8_t read_value(void) {
  uint8_t v = read_value_at(u2_data_address);
  auto_increment();
  return v;
}

static void write_common_register(uint16_t address, uint8_t value) {
  if (address == W5100_MR) {
    if (value & W5100_MR_RST)
      u2_reset();
    else
      u2_mode_register = value;
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
}

/* Push received data into socket i's RX buffer. UDP: write 4B IP + 2B port + 2B len (big-endian) then payload. TCP: payload only. */
static void u2_push_rx(int socket_i, const uint8_t *data, uint16_t len, int is_udp, uint32_t src_ip, uint16_t src_port) {
  if (socket_i < 0 || socket_i >= W5100_NUM_SOCKETS || !data) return;
  u2_socket_t *s = &u2_sockets[socket_i];
  uint16_t size = s->receive_size;
  if (size == 0) return;
  uint16_t mask = size - 1;
  uint16_t base = s->receive_base;
  uint16_t total = len;
  if (is_udp) total += 8;  /* 4 + 2 + 2 */
  if (get_rx_rsr(socket_i) + total > size) return;  /* no room, drop */
  if (is_udp) {
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)(src_ip >> 24);
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)(src_ip >> 16);
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)(src_ip >> 8);
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)src_ip;
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)(src_port >> 8);
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)src_port;
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)(len >> 8);
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
    u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)len;
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
  }
  for (uint16_t k = 0; k < len; k++) {
    u2_memory[base + (s->sn_rx_wr & mask)] = data[k];
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
  }
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
  if (get_rx_rsr(socket_i) + total > size) return;
  u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)(len >> 8);
  s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
  u2_memory[base + (s->sn_rx_wr & mask)] = (uint8_t)len;
  s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
  for (uint16_t k = 0; k < len; k++) {
    u2_memory[base + (s->sn_rx_wr & mask)] = data[k];
    s->sn_rx_wr = (s->sn_rx_wr + 1) & mask;
  }
}

/* Read TX buffer data between rd and wr and send via network */
static void send_data(int i) {
  const u2_socket_t *s = &u2_sockets[i];
  uint16_t buf_size = s->transmit_size;
  if (buf_size == 0) return;
  uint16_t mask = buf_size - 1;
  const uint8_t *r = &u2_memory[s->register_address];
  uint16_t rd = read_net16(r + W5100_SN_TX_RD0) & mask;
  uint16_t wr = read_net16(r + W5100_SN_TX_WR0) & mask;
  int data_len = (int)wr - (int)rd;
  if (data_len <= 0) data_len += buf_size;
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
    U2_Net_SendUdp(i, buf, (uint16_t)n, dip, dport);
  } else if (status == W5100_SN_SR_ESTABLISHED) {
    uint8_t buf[2048];
    int n = data_len;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    for (int j = 0; j < n; j++)
      buf[j] = u2_memory[base + ((rd + j) & mask)];
    U2_Net_SendTcp(i, buf, (uint16_t)n);
  } else if (status == W5100_SN_SR_SOCK_MACRAW) {
    uint8_t buf[1518];
    int n = data_len;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    for (int j = 0; j < n; j++)
      buf[j] = u2_memory[base + ((rd + j) & mask)];
    U2_Net_SendMacraw(i, buf, (uint16_t)n);
  }
  /* Advance TX_RD to TX_WR */
  u2_memory[s->register_address + W5100_SN_TX_RD0] = (uint8_t)(wr >> 8);
  u2_memory[s->register_address + W5100_SN_TX_RD1] = (uint8_t)wr;
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
      case W5100_SN_MR_UDP:
        if (U2_Net_OpenUdp(i, port) == 0) { /* ok */ }
        else u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
        break;
      case W5100_SN_MR_TCP:
        if (U2_Net_OpenTcp(i) == 0) u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_SOCK_INIT;
        else u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
        break;
      case W5100_SN_MR_MACRAW:
        if (U2_Net_OpenMacraw(i) == 0) u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_SOCK_MACRAW;
        else u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
        break;
      default:
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
      if (U2_Net_ConnectTcpEx(i, dip, dport) != 0)
        u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      break;
    }
    case W5100_SN_CR_LISTEN: {
      uint16_t port = (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT0] << 8
                    | (uint16_t)u2_memory[(address & 0xFF00) + W5100_SN_PORT1];
      if (U2_Net_ListenTcp(i, port) != 0)
        u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      break;
    }
    case W5100_SN_CR_CLOSE:
    case W5100_SN_CR_DISCON:
      U2_Net_Close(i);
      u2_memory[(address & 0xFF00) + W5100_SN_SR] = W5100_SN_SR_CLOSED;
      break;
    case W5100_SN_CR_SEND:
      send_data(i);
      break;
    case W5100_SN_CR_RECV:
      U2_Net_RecvConfirm(i);
      break;
    default:
      break;
    }
  }
}

static void write_value_at(uint16_t address, uint8_t value) {
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

static void write_value(uint8_t value) {
  write_value_at(u2_data_address, value);
  auto_increment();
}

void U2_Init(void) {
  u2_data_address = 0;
  U2_Net_Init(u2_push_rx, u2_push_rx_macraw);
  u2_reset();
}

void U2_Poll(void) {
  U2_Net_Poll();
}

void U2_HandleBusAccess(uint32_t busdata, uint8_t *read_byte_out) {
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
      else
        u2_mode_register = data;
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
}

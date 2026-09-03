/**
 * U2 activity monitor implementation (see u2_monitor.h).
 */
#ifndef U2_ACTIVITY_MONITOR
#define U2_ACTIVITY_MONITOR 0
#endif

#include "u2_monitor.h"

#ifndef U2_IP65_CHECKPOINT
#define U2_IP65_CHECKPOINT 0
#endif

#if U2_ACTIVITY_MONITOR

#include <stdio.h>
#include "pico/critical_section.h"
#include "pico/time.h"

#define U2_MON_RING 256
/* Flush budget per core-0 iteration. printf here goes to a blocking UART, so at 115200 the stock
 * 128 lines (~10 KB) cost core 0 roughly 0.9 s per call — NetworkPump_PollOnce is starved and TCP
 * handshakes time out, which by itself makes the Debug build unable to connect (§1cz/§1de).
 * Override with -DU2_MON_FLUSH_MAX=n (and raise PICO_DEFAULT_UART_BAUD_RATE) to keep the trace
 * while leaving core 0 enough time to service lwIP. */
#ifndef U2_MON_FLUSH_MAX
#define U2_MON_FLUSH_MAX 128
#endif

enum {
  U2M_BUS = 1,
  U2M_RESET,
  U2M_SOCK_OPEN,
  U2M_SOCK_CONN,
  U2M_SOCK_LISTEN,
  U2M_SOCK_CLOSE,
  U2M_SOCK_SNDRCV,
  U2M_NET_UDPSND,
  U2M_NET_TCPSND,
  U2M_NET_UDPRX,
  U2M_NET_TCPRX,
  U2M_NET_MACRX,
  U2M_NET_RXDROP,
  U2M_NET_MACTX,
  U2M_NET_MACTX_PTRS,
  U2M_RECV_STALL,
  U2M_RECV_RESYNC,
  U2M_RECV_STALL_DBG,
  U2M_RECV_STALL_RING,
  U2M_MODE_LINE,
  U2M_DATA_READ_TRACE,
  U2M_DATA_WRITE_TRACE,
  U2M_CHECKPOINT,
};

typedef struct {
  uint32_t t_us;
  uint8_t op;
  uint8_t a0;
  uint8_t a1;
  uint8_t a2;
  uint16_t w0;
  uint32_t w1;
  uint32_t w2;
} u2_mon_evt_t;

static critical_section_t u2_mon_cs;
static u2_mon_evt_t u2_mon_ring[U2_MON_RING];
static uint32_t u2_mon_head;
static uint32_t u2_mon_tail;
static uint32_t u2_mon_dropped;

static void u2_mon_push(const u2_mon_evt_t *e) {
  critical_section_enter_blocking(&u2_mon_cs);
  uint32_t next = (u2_mon_head + 1u) % U2_MON_RING;
  if (next == u2_mon_tail) {
    u2_mon_dropped++;
    critical_section_exit(&u2_mon_cs);
    return;
  }
  u2_mon_ring[u2_mon_head] = *e;
  u2_mon_head = next;
  critical_section_exit(&u2_mon_cs);
}

static const char *loc_name(unsigned loc) {
  switch (loc & 3u) {
  case 0:
    return "MODE";
  case 1:
    return "ADDRHI";
  case 2:
    return "ADDRLO";
  case 3:
  default:
    return "DATA";
  }
}

static void u2_mon_format_one(const u2_mon_evt_t *e) {
  switch (e->op) {
  case U2M_BUS:
    printf("[u2m] %lu bus %s %s busdata=0x%03lX byte=0x%02X ptr=0x%04lX MR=0x%02X\n", (unsigned long)e->t_us,
           e->a0 ? "RD" : "WR", loc_name(e->a1), (unsigned long)(e->w1 & 0xFFFu), (unsigned)e->a2,
           (unsigned long)e->w2, (unsigned)e->w0);
    break;
  case U2M_RESET:
    printf("[u2m] %lu reset\n", (unsigned long)e->t_us);
    break;
  case U2M_SOCK_OPEN:
    printf("[u2m] %lu sock%d OPEN mr=0x%02X port=%u %s\n", (unsigned long)e->t_us, (int)e->a0, (unsigned)e->a1,
           (unsigned)e->w0, e->a2 ? "ok" : "FAIL");
    break;
  case U2M_SOCK_CONN:
    printf("[u2m] %lu sock%d CONNECT %u.%u.%u.%u:%u %s\n", (unsigned long)e->t_us, (int)e->a0,
           (unsigned)(e->w1 >> 24) & 0xff, (unsigned)(e->w1 >> 16) & 0xff, (unsigned)(e->w1 >> 8) & 0xff,
           (unsigned)e->w1 & 0xff, (unsigned)e->w0, e->a2 ? "ok" : "FAIL");
    break;
  case U2M_SOCK_LISTEN:
    printf("[u2m] %lu sock%d LISTEN port=%u %s\n", (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0,
           e->a2 ? "ok" : "FAIL");
    break;
  case U2M_SOCK_CLOSE:
    printf("[u2m] %lu sock%d CLOSE/DISCON\n", (unsigned long)e->t_us, (int)e->a0);
    break;
  case U2M_SOCK_SNDRCV:
    printf("[u2m] %lu sock%d %s\n", (unsigned long)e->t_us, (int)e->a0, e->a1 ? "SEND" : "RECV");
    break;
  case U2M_NET_UDPSND:
    printf("[u2m] %lu net sock%d UDP tx len=%u -> %u.%u.%u.%u:%u\n", (unsigned long)e->t_us, (int)e->a0,
           (unsigned)e->w0, (unsigned)(e->w1 >> 24) & 0xff, (unsigned)(e->w1 >> 16) & 0xff,
           (unsigned)(e->w1 >> 8) & 0xff, (unsigned)e->w1 & 0xff, (unsigned)e->w2);
    break;
  case U2M_NET_TCPSND:
    printf("[u2m] %lu net sock%d TCP tx len=%u\n", (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0);
    break;
  case U2M_NET_UDPRX:
    printf("[u2m] %lu net sock%d UDP rx len=%u <- %u.%u.%u.%u:%u\n", (unsigned long)e->t_us, (int)e->a0,
           (unsigned)e->w0, (unsigned)(e->w1 >> 24) & 0xff, (unsigned)(e->w1 >> 16) & 0xff,
           (unsigned)(e->w1 >> 8) & 0xff, (unsigned)e->w1 & 0xff, (unsigned)e->w2);
    break;
  case U2M_NET_TCPRX:
    printf("[u2m] %lu net sock%d TCP rx len=%u\n", (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0);
    break;
  case U2M_NET_MACRX:
    printf("[u2m] %lu net sock%d MACRAW rx len=%u\n", (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0);
    break;
  case U2M_NET_RXDROP: {
    const char *proto = "?";
    const char *reason = "?";
    switch (e->a1) {
    case 1: proto = "UDP"; break;
    case 2: proto = "TCP"; break;
    case 3: proto = "MACRAW"; break;
    default: break;
    }
    switch (e->a2) {
    case 1: reason = "no-room"; break;
    case 2: reason = "partial"; break;
    case 3: reason = "frame-too-big"; break;
    case 4: reason = "size-map-clamped"; break;
    default: break;
    }
    printf("[u2m] %lu net sock%d %s rx %s offered=%u accepted=%u free=%u ring=%u\n", (unsigned long)e->t_us,
           (int)e->a0, proto, reason, (unsigned)e->w0, (unsigned)e->w1, (unsigned)(e->w2 & 0xFFFFu),
           (unsigned)(e->w2 >> 16));
    break;
  }
  case U2M_NET_MACTX:
    printf("[u2m] %lu net sock%d MACRAW tx len=%u\n", (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0);
    break;
  case U2M_NET_MACTX_PTRS:
    printf("[u2m] %lu net sock%d MACRAW ptrs len=%u%s rd=0x%04X wr=0x%04X rdm=0x%02X wrm=0x%02X\n",
           (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0, (e->w0 > 1518u) ? " OVERSIZE" : "",
           (unsigned)e->w1 & 0xFFFFu, (unsigned)e->w2 & 0xFFFFu, (unsigned)e->a1, (unsigned)e->a2);
    break;
  case U2M_RECV_STALL:
    printf("[u2m] %lu sock%d RECV STALL rsr=%u rd=0x%04X rd_off=0x%04X hdr=%02X%02X (host RX_RD frozen)\n",
           (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0, (unsigned)e->w1 & 0xFFFFu,
           (unsigned)e->w2 & 0xFFFFu, (unsigned)e->a1, (unsigned)e->a2);
    break;
  case U2M_RECV_RESYNC:
    printf("[u2m] %lu sock%d RECV RESYNC rsr=%u rd=0x%04X -> wr=0x%04X hdr=%02X%02X (wedge cleared, tail discarded)\n",
           (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0, (unsigned)e->w1 & 0xFFFFu,
           (unsigned)e->w2 & 0xFFFFu, (unsigned)e->a1, (unsigned)e->a2);
    break;
  case U2M_RECV_STALL_DBG:
    printf("[u2m] %lu sock%d dbg e66cf7 A/C last_wire=%u rec_off=0x%04X hdr_at_rec=%04X match=%u\n",
           (unsigned long)e->t_us, (int)e->a0, (unsigned)e->w0, (unsigned)e->w1 & 0xFFFFu,
           (unsigned)(e->w1 >> 16), (unsigned)e->a1);
    break;
  case U2M_RECV_STALL_RING:
    printf("[u2m] %lu sock%d dbg e66cf7 ring8=%08lX%08lX\n", (unsigned long)e->t_us, (int)e->a0,
           (unsigned long)e->w1, (unsigned long)e->w2);
    break;
  case U2M_MODE_LINE:
    printf("[u2] mode=0x%02X (AI=%d IND=%d)\n", (unsigned)e->a1, (e->a1 & 0x02) ? 1 : 0, (e->a1 & 0x01) ? 1 : 0);
    break;
  case U2M_DATA_READ_TRACE:
    printf("[u2] DATA read addr=0x%04X -> 0x%02X (MR=0x%02X)\n", (unsigned)e->w2, (unsigned)e->a2,
           (unsigned)e->a1);
    break;
  case U2M_DATA_WRITE_TRACE:
    printf("[u2] DATA write addr=0x%04X <- 0x%02X (MR=0x%02X)\n", (unsigned)e->w2, (unsigned)e->a2,
           (unsigned)e->a1);
    break;
  case U2M_CHECKPOINT:
    printf("[u2] ck=%u\n", (unsigned)e->a1);
    break;
  default:
    printf("[u2m] %lu op=%u\n", (unsigned long)e->t_us, (unsigned)e->op);
    break;
  }
}

void U2_MonInit(void) {
  critical_section_init(&u2_mon_cs);
  critical_section_enter_blocking(&u2_mon_cs);
  u2_mon_head = u2_mon_tail = 0;
  u2_mon_dropped = 0;
  critical_section_exit(&u2_mon_cs);
  printf("[u2m] U2 activity monitor on; bus+socket+net events; ring=%d flush<=%d/call\n", U2_MON_RING,
         U2_MON_FLUSH_MAX);
}

void U2_MonPollFlush(void) {
  uint32_t dropped_report = 0;
  critical_section_enter_blocking(&u2_mon_cs);
  dropped_report = u2_mon_dropped;
  u2_mon_dropped = 0;
  critical_section_exit(&u2_mon_cs);
  if (dropped_report)
    printf("[u2m] WARNING dropped %lu monitor events (ring full)\n", (unsigned long)dropped_report);

  for (int n = 0; n < U2_MON_FLUSH_MAX; n++) {
    u2_mon_evt_t e;
    critical_section_enter_blocking(&u2_mon_cs);
    if (u2_mon_tail == u2_mon_head) {
      critical_section_exit(&u2_mon_cs);
      break;
    }
    e = u2_mon_ring[u2_mon_tail];
    u2_mon_tail = (u2_mon_tail + 1u) % U2_MON_RING;
    critical_section_exit(&u2_mon_cs);
    u2_mon_format_one(&e);
  }
}

void U2_MonBus(int is_read, unsigned loc, uint32_t busdata, uint8_t data_byte, uint32_t data_ptr,
               unsigned mode_reg) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_BUS,
                     .a0 = (uint8_t)(is_read ? 1u : 0u),
                     .a1 = (uint8_t)(loc & 3u),
                     .a2 = data_byte,
                     .w0 = (uint16_t)(mode_reg & 0xFFu),
                     .w1 = busdata & 0xFFFu,
                     .w2 = data_ptr & 0xFFFFu};
  u2_mon_push(&ev);
}

void U2_MonQueueModeLine(uint8_t mr) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_MODE_LINE, .a1 = mr};
  u2_mon_push(&ev);
}

void U2_MonDataReadTrace(uint16_t addr, uint8_t val, uint8_t mr) {
  u2_mon_evt_t ev = {
      .t_us = time_us_32(), .op = U2M_DATA_READ_TRACE, .a1 = mr, .a2 = val, .w2 = (uint32_t)addr};
  u2_mon_push(&ev);
}

void U2_MonDataWriteTrace(uint16_t addr, uint8_t val, uint8_t mr) {
  u2_mon_evt_t ev = {
      .t_us = time_us_32(), .op = U2M_DATA_WRITE_TRACE, .a1 = mr, .a2 = val, .w2 = (uint32_t)addr};
  u2_mon_push(&ev);
}

void U2_MonCheckpoint(int n) {
  if (U2_IP65_CHECKPOINT == 0 || n != U2_IP65_CHECKPOINT)
    return;
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_CHECKPOINT, .a1 = (uint8_t)n};
  u2_mon_push(&ev);
}

void U2_MonReset(void) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_RESET};
  u2_mon_push(&ev);
}

void U2_MonSockOpen(int sock, uint8_t mr, uint16_t port, int ok) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_SOCK_OPEN,
                     .a0 = (uint8_t)sock,
                     .a1 = mr,
                     .a2 = (uint8_t)(ok ? 1u : 0u),
                     .w0 = port,
                     .w1 = 0,
                     .w2 = 0};
  u2_mon_push(&ev);
}

void U2_MonSockConnect(int sock, uint32_t dip, uint16_t dport, int ok) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_SOCK_CONN,
                     .a0 = (uint8_t)sock,
                     .a1 = 0,
                     .a2 = (uint8_t)(ok ? 1u : 0u),
                     .w0 = dport,
                     .w1 = dip,
                     .w2 = 0};
  u2_mon_push(&ev);
}

void U2_MonSockListen(int sock, uint16_t port, int ok) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_SOCK_LISTEN,
                     .a0 = (uint8_t)sock,
                     .a1 = 0,
                     .a2 = (uint8_t)(ok ? 1u : 0u),
                     .w0 = port,
                     .w1 = 0,
                     .w2 = 0};
  u2_mon_push(&ev);
}

void U2_MonSockClose(int sock) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_SOCK_CLOSE, .a0 = (uint8_t)sock};
  u2_mon_push(&ev);
}

void U2_MonSockSendRecv(int sock, int is_send) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_SOCK_SNDRCV, .a0 = (uint8_t)sock, .a1 = (uint8_t)(is_send ? 1u : 0u)};
  u2_mon_push(&ev);
}

void U2_MonNetUdpSend(int sock, uint32_t dip, uint16_t dport, uint16_t len) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_NET_UDPSND,
                     .a0 = (uint8_t)sock,
                     .w0 = len,
                     .w1 = dip,
                     .w2 = dport};
  u2_mon_push(&ev);
}

void U2_MonNetTcpSend(int sock, uint16_t len) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_NET_TCPSND, .a0 = (uint8_t)sock, .w0 = len};
  u2_mon_push(&ev);
}

void U2_MonNetRxUdp(int sock, uint16_t len, uint32_t src_ip_host, uint16_t src_port) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_NET_UDPRX,
                     .a0 = (uint8_t)sock,
                     .w0 = len,
                     .w1 = src_ip_host,
                     .w2 = src_port};
  u2_mon_push(&ev);
}

void U2_MonNetRxTcp(int sock, uint16_t len) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_NET_TCPRX, .a0 = (uint8_t)sock, .w0 = len};
  u2_mon_push(&ev);
}

void U2_MonNetRxMacraw(int sock, uint16_t len) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_NET_MACRX, .a0 = (uint8_t)sock, .w0 = len};
  u2_mon_push(&ev);
}

#ifndef U2_MON_RXDROP_MIN_INTERVAL_US
#define U2_MON_RXDROP_MIN_INTERVAL_US 100000u
#endif

void U2_MonNetRxDrop(int sock, uint8_t proto, uint8_t reason, uint16_t offered, uint16_t accepted, uint16_t free_bytes,
                     uint16_t ring_size) {
  static uint32_t u2_mon_rxdrop_last_us[4][6];
  if (sock >= 0 && sock < 4 && reason > 0 && reason < 6) {
    uint32_t now = time_us_32();
    if ((uint32_t)(now - u2_mon_rxdrop_last_us[sock][reason]) < U2_MON_RXDROP_MIN_INTERVAL_US)
      return;
    u2_mon_rxdrop_last_us[sock][reason] = now;
  }
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_NET_RXDROP,
                     .a0 = (uint8_t)sock,
                     .a1 = proto,
                     .a2 = reason,
                     .w0 = offered,
                     .w1 = accepted,
                     .w2 = ((uint32_t)ring_size << 16) | (uint32_t)free_bytes};
  u2_mon_push(&ev);
}

void U2_MonNetMacrawTx(int sock, uint16_t len) {
  u2_mon_evt_t ev = {.t_us = time_us_32(), .op = U2M_NET_MACTX, .a0 = (uint8_t)sock, .w0 = len};
  u2_mon_push(&ev);
}

void U2_MonNetMacrawTxPtrs(int sock, uint16_t len, uint16_t rd_full, uint16_t wr_full, uint16_t rd_masked,
                           uint16_t wr_masked) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_NET_MACTX_PTRS,
                     .a0 = (uint8_t)sock,
                     .a1 = (uint8_t)(rd_masked & 0xFFu),
                     .a2 = (uint8_t)(wr_masked & 0xFFu),
                     .w0 = len,
                     .w1 = rd_full,
                     .w2 = wr_full};
  u2_mon_push(&ev);
}

void U2_MonRecvStall(int sock, uint16_t rsr, uint16_t rd_full, uint16_t wr_off, uint8_t h0, uint8_t h1) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_RECV_STALL,
                     .a0 = (uint8_t)sock,
                     .a1 = h0,
                     .a2 = h1,
                     .w0 = rsr,
                     .w1 = rd_full,
                     .w2 = wr_off};
  u2_mon_push(&ev);
}

void U2_MonRecvResync(int sock, uint16_t rsr, uint16_t rd_full, uint16_t wr_full, uint8_t h0, uint8_t h1) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_RECV_RESYNC,
                     .a0 = (uint8_t)sock,
                     .a1 = h0,
                     .a2 = h1,
                     .w0 = rsr,
                     .w1 = rd_full,
                     .w2 = wr_full};
  u2_mon_push(&ev);
}

void U2_MonRecvStallDbg(int sock, uint16_t last_wire, uint16_t rec_off, uint16_t hdr_at_rec, uint8_t match,
                        uint32_t ring4a, uint32_t ring4b) {
  u2_mon_evt_t ev = {.t_us = time_us_32(),
                     .op = U2M_RECV_STALL_DBG,
                     .a0 = (uint8_t)sock,
                     .a1 = match,
                     .w0 = last_wire,
                     .w1 = (uint32_t)rec_off | ((uint32_t)hdr_at_rec << 16),
                     .w2 = 0};
  u2_mon_push(&ev);
  ev.op = U2M_RECV_STALL_RING;
  ev.w1 = ring4a;
  ev.w2 = ring4b;
  u2_mon_push(&ev);
}

#else /* !U2_ACTIVITY_MONITOR */

#include <stddef.h>

void U2_MonInit(void) { (void)0; }
void U2_MonPollFlush(void) { (void)0; }
void U2_MonBus(int is_read, unsigned loc, uint32_t busdata, uint8_t data_byte, uint32_t data_ptr,
               unsigned mode_reg) {
  (void)is_read;
  (void)loc;
  (void)busdata;
  (void)data_byte;
  (void)data_ptr;
  (void)mode_reg;
}
void U2_MonReset(void) { (void)0; }
void U2_MonSockOpen(int sock, uint8_t mr, uint16_t port, int ok) {
  (void)sock;
  (void)mr;
  (void)port;
  (void)ok;
}
void U2_MonSockConnect(int sock, uint32_t dip, uint16_t dport, int ok) {
  (void)sock;
  (void)dip;
  (void)dport;
  (void)ok;
}
void U2_MonSockListen(int sock, uint16_t port, int ok) {
  (void)sock;
  (void)port;
  (void)ok;
}
void U2_MonSockClose(int sock) { (void)sock; }
void U2_MonSockSendRecv(int sock, int is_send) {
  (void)sock;
  (void)is_send;
}
void U2_MonNetUdpSend(int sock, uint32_t dip, uint16_t dport, uint16_t len) {
  (void)sock;
  (void)dip;
  (void)dport;
  (void)len;
}
void U2_MonNetTcpSend(int sock, uint16_t len) {
  (void)sock;
  (void)len;
}
void U2_MonNetRxUdp(int sock, uint16_t len, uint32_t src_ip_host, uint16_t src_port) {
  (void)sock;
  (void)len;
  (void)src_ip_host;
  (void)src_port;
}
void U2_MonNetRxTcp(int sock, uint16_t len) {
  (void)sock;
  (void)len;
}
void U2_MonNetRxMacraw(int sock, uint16_t len) {
  (void)sock;
  (void)len;
}
void U2_MonNetRxDrop(int sock, uint8_t proto, uint8_t reason, uint16_t offered, uint16_t accepted, uint16_t free_bytes,
                     uint16_t ring_size) {
  (void)sock;
  (void)proto;
  (void)reason;
  (void)offered;
  (void)accepted;
  (void)free_bytes;
  (void)ring_size;
}
void U2_MonNetMacrawTx(int sock, uint16_t len) {
  (void)sock;
  (void)len;
}
void U2_MonNetMacrawTxPtrs(int sock, uint16_t len, uint16_t rd_full, uint16_t wr_full, uint16_t rd_masked,
                           uint16_t wr_masked) {
  (void)sock;
  (void)len;
  (void)rd_full;
  (void)wr_full;
  (void)rd_masked;
  (void)wr_masked;
}
void U2_MonRecvStall(int sock, uint16_t rsr, uint16_t rd_full, uint16_t wr_off, uint8_t h0, uint8_t h1) {
  (void)sock;
  (void)rsr;
  (void)rd_full;
  (void)wr_off;
  (void)h0;
  (void)h1;
}
void U2_MonRecvResync(int sock, uint16_t rsr, uint16_t rd_full, uint16_t wr_full, uint8_t h0, uint8_t h1) {
  (void)sock;
  (void)rsr;
  (void)rd_full;
  (void)wr_full;
  (void)h0;
  (void)h1;
}
void U2_MonRecvStallDbg(int sock, uint16_t last_wire, uint16_t rec_off, uint16_t hdr_at_rec, uint8_t match,
                        uint32_t ring4a, uint32_t ring4b) {
  (void)sock;
  (void)last_wire;
  (void)rec_off;
  (void)hdr_at_rec;
  (void)match;
  (void)ring4a;
  (void)ring4b;
}

void U2_MonQueueModeLine(uint8_t mr) { (void)mr; }
void U2_MonDataReadTrace(uint16_t addr, uint8_t val, uint8_t mr) {
  (void)addr;
  (void)val;
  (void)mr;
}
void U2_MonDataWriteTrace(uint16_t addr, uint8_t val, uint8_t mr) {
  (void)addr;
  (void)val;
  (void)mr;
}
void U2_MonCheckpoint(int n) { (void)n; }

#endif /* U2_ACTIVITY_MONITOR */

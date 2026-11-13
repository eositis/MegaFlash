/**
 * Uthernet II activity monitor: queues events from bus (core 1) and net (lwIP),
 * flushes to UART from core 0 only (e.g. next to NetworkPump_PollOnce in main).
 * Do not call U2_MonPollFlush from core 1 — UART/cyw43 async_context will panic.
 *
 * Enabled when U2_ACTIVITY_MONITOR=1 (CMake Debug). When 0 (Release) every call is an
 * inline no-op: the bus handler used to call ~10 of these through flash veneers on
 * every command (section 1ev), which put XIP fetches on the timing-critical path.
 */
#ifndef U2_MONITOR_H
#define U2_MONITOR_H

#include <stdint.h>

#ifndef U2_ACTIVITY_MONITOR
#define U2_ACTIVITY_MONITOR 0
#endif

#if U2_ACTIVITY_MONITOR

#ifdef __cplusplus
extern "C" {
#endif

void U2_MonInit(void);
/** Call from core 0 only (after NetworkPump_PollOnce); drains queue to UART (bounded per call). */
void U2_MonPollFlush(void);

/** Queued [u2] lines — do not printf from U2_HandleBusAccess; flush runs in U2_Poll. */
void U2_MonQueueModeLine(uint8_t mr);
void U2_MonDataReadTrace(uint16_t addr, uint8_t val, uint8_t mr);
void U2_MonDataWriteTrace(uint16_t addr, uint8_t val, uint8_t mr);
/** Single checkpoint line [u2] ck=n when U2_IP65_CHECKPOINT==n (CMake); queued, not printf on bus. */
void U2_MonCheckpoint(int n);

/** One C0C4–C0C7 bus cycle after U2_HandleBusAccess completes. */
void U2_MonBus(int is_read, unsigned loc, uint32_t busdata, uint8_t data_byte,
               uint32_t data_ptr, unsigned mode_reg);

void U2_MonReset(void);

void U2_MonSockOpen(int sock, uint8_t mr, uint16_t port, int ok_udp_tcp_mac);
void U2_MonSockConnect(int sock, uint32_t dip, uint16_t dport, int ok);
void U2_MonSockListen(int sock, uint16_t port, int ok);
void U2_MonSockClose(int sock);
void U2_MonSockSendRecv(int sock, int is_send /*1=SEND 0=RECV*/);

void U2_MonNetUdpSend(int sock, uint32_t dip, uint16_t dport, uint16_t len);
void U2_MonNetTcpSend(int sock, uint16_t len);
void U2_MonNetRxUdp(int sock, uint16_t len, uint32_t src_ip_host, uint16_t src_port);
void U2_MonNetRxTcp(int sock, uint16_t len);
void U2_MonNetRxMacraw(int sock, uint16_t len);
/** RX enqueue outcome telemetry (proto: 1=UDP 2=TCP 3=MACRAW; reason: 1=no-room 2=partial 3=frame-too-big 4=size-map-clamped). */
void U2_MonNetRxDrop(int sock, uint8_t proto, uint8_t reason, uint16_t offered, uint16_t accepted, uint16_t free_bytes,
                     uint16_t ring_size);
void U2_MonNetMacrawTx(int sock, uint16_t len);
void U2_MonNetMacrawTxPtrs(int sock, uint16_t len, uint16_t rd_full, uint16_t wr_full, uint16_t rd_masked,
                           uint16_t wr_masked);
/** Diagnostic: host issued RECV repeatedly without advancing Sn_RX_RD (storm signature). Rate-limited. */
void U2_MonRecvStall(int sock, uint16_t rsr, uint16_t rd_full, uint16_t wr_off, uint8_t h0, uint8_t h1);
void U2_MonRecvResync(int sock, uint16_t rsr, uint16_t rd_full, uint16_t wr_full, uint8_t h0, uint8_t h1);
/** First-STALL only: last MACRAW record vs ring (A/C). match=1 if hdr at (wr-last_wire) still equals last_wire. */
void U2_MonRecvStallDbg(int sock, uint16_t last_wire, uint16_t rec_off, uint16_t hdr_at_rec, uint8_t match,
                        uint32_t ring4a, uint32_t ring4b);

#ifdef __cplusplus
}
#endif

#else /* !U2_ACTIVITY_MONITOR: inline no-ops, so the bus path never leaves SRAM for them */

#define U2_MON_NOP static inline __attribute__((always_inline, unused))

U2_MON_NOP void U2_MonInit(void) {}
U2_MON_NOP void U2_MonPollFlush(void) {}
U2_MON_NOP void U2_MonQueueModeLine(uint8_t mr) { (void)mr; }
U2_MON_NOP void U2_MonDataReadTrace(uint16_t a, uint8_t v, uint8_t m) { (void)a; (void)v; (void)m; }
U2_MON_NOP void U2_MonDataWriteTrace(uint16_t a, uint8_t v, uint8_t m) { (void)a; (void)v; (void)m; }
U2_MON_NOP void U2_MonCheckpoint(int n) { (void)n; }
U2_MON_NOP void U2_MonBus(int r, unsigned l, uint32_t b, uint8_t d, uint32_t p, unsigned m) {
  (void)r; (void)l; (void)b; (void)d; (void)p; (void)m;
}
U2_MON_NOP void U2_MonReset(void) {}
U2_MON_NOP void U2_MonSockOpen(int s, uint8_t mr, uint16_t port, int ok) { (void)s; (void)mr; (void)port; (void)ok; }
U2_MON_NOP void U2_MonSockConnect(int s, uint32_t dip, uint16_t dport, int ok) { (void)s; (void)dip; (void)dport; (void)ok; }
U2_MON_NOP void U2_MonSockListen(int s, uint16_t port, int ok) { (void)s; (void)port; (void)ok; }
U2_MON_NOP void U2_MonSockClose(int s) { (void)s; }
U2_MON_NOP void U2_MonSockSendRecv(int s, int is_send) { (void)s; (void)is_send; }
U2_MON_NOP void U2_MonNetUdpSend(int s, uint32_t dip, uint16_t dport, uint16_t len) { (void)s; (void)dip; (void)dport; (void)len; }
U2_MON_NOP void U2_MonNetTcpSend(int s, uint16_t len) { (void)s; (void)len; }
U2_MON_NOP void U2_MonNetRxUdp(int s, uint16_t len, uint32_t ip, uint16_t port) { (void)s; (void)len; (void)ip; (void)port; }
U2_MON_NOP void U2_MonNetRxTcp(int s, uint16_t len) { (void)s; (void)len; }
U2_MON_NOP void U2_MonNetRxMacraw(int s, uint16_t len) { (void)s; (void)len; }
U2_MON_NOP void U2_MonNetRxDrop(int s, uint8_t proto, uint8_t reason, uint16_t offered, uint16_t accepted,
                                uint16_t free_bytes, uint16_t ring_size) {
  (void)s; (void)proto; (void)reason; (void)offered; (void)accepted; (void)free_bytes; (void)ring_size;
}
U2_MON_NOP void U2_MonNetMacrawTx(int s, uint16_t len) { (void)s; (void)len; }
U2_MON_NOP void U2_MonNetMacrawTxPtrs(int s, uint16_t len, uint16_t rdf, uint16_t wrf, uint16_t rdm, uint16_t wrm) {
  (void)s; (void)len; (void)rdf; (void)wrf; (void)rdm; (void)wrm;
}
U2_MON_NOP void U2_MonRecvStall(int s, uint16_t rsr, uint16_t rdf, uint16_t wro, uint8_t h0, uint8_t h1) {
  (void)s; (void)rsr; (void)rdf; (void)wro; (void)h0; (void)h1;
}
U2_MON_NOP void U2_MonRecvResync(int s, uint16_t rsr, uint16_t rdf, uint16_t wrf, uint8_t h0, uint8_t h1) {
  (void)s; (void)rsr; (void)rdf; (void)wrf; (void)h0; (void)h1;
}
U2_MON_NOP void U2_MonRecvStallDbg(int s, uint16_t lw, uint16_t ro, uint16_t ha, uint8_t m, uint32_t a, uint32_t b) {
  (void)s; (void)lw; (void)ro; (void)ha; (void)m; (void)a; (void)b;
}

#endif /* U2_ACTIVITY_MONITOR */

#endif /* U2_MONITOR_H */

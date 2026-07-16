/**
 * Uthernet II activity monitor: queues events from bus (core 1) and net (lwIP),
 * flushes to UART from core 0 only (e.g. next to NetworkPump_PollOnce in main).
 * Do not call U2_MonPollFlush from core 1 — UART/cyw43 async_context will panic.
 *
 * Enabled when U2_ACTIVITY_MONITOR=1 (CMake Debug). Stubs when 0.
 */
#ifndef U2_MONITOR_H
#define U2_MONITOR_H

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif /* U2_MONITOR_H */

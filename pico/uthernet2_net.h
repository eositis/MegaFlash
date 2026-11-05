/**
 * Uthernet II network layer (lwIP TCP/UDP).
 * Call U2_Net_Init() with a push_rx callback; then use U2_Net_* from uthernet2.
 */
#ifndef _UTHERNET2_NET_H
#define _UTHERNET2_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback: push received data into W5100 RX buffer for socket i.
 * is_udp: 1 = UDP (4B IP + 2B port + 2B len + payload);
 *         2 = IPRAW (4B IP + 2B len + payload);
 *         0 = TCP (payload only).
 * src_ip: IPv4 in host order (for UDP/IPRAW header); src_port host order.
 * Return: accepted payload bytes (for TCP flow control this may be < len). */
typedef uint16_t (*u2_push_rx_fn)(int socket_i, const uint8_t *data, uint16_t len,
                                  int is_udp, uint32_t src_ip, uint16_t src_port);
/* Callback: push MACRAW frame into socket i's RX buffer (2-byte length big-endian then frame). */
typedef void (*u2_push_rx_macraw_fn)(int socket_i, const uint8_t *data, uint16_t len);

void U2_Net_Init(u2_push_rx_fn push_rx, u2_push_rx_macraw_fn push_rx_macraw);

void U2_Net_Close(int i);
/** Graceful TCP close (FIN). CLOSE uses U2_Net_Close (abort). Core 0. Returns 0 if accepted. */
int  U2_Net_DisconTcp(int i);
int  U2_Net_OpenUdp(int i, uint16_t local_port);
int  U2_Net_OpenTcp(int i);
int  U2_Net_OpenIpraw(int i, uint8_t proto);
int  U2_Net_OpenMacraw(int i);
/** Sn_MR ND: disable Nagle on the socket's TCP pcb. Core 0. */
void U2_Net_SetTcpNoDelay(int i, int enable);
/** Returns 0 if accepted (queued or sent), -1 if queue full or linkoutput failed. */
int  U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len);
/** Feed a received raw Ethernet frame into socket i (MACRAW RX). Call from driver hook if available. */
void U2_Net_FeedMacrawRx(int i, const uint8_t *data, uint16_t len);
int  U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port, uint16_t local_port);
int  U2_Net_ListenTcp(int i, uint16_t local_port);
/** Send a UDP datagram copied from the socket TX ring (handles wrap; ring_size must be power of two).
 * Copies directly into the pbuf so no large intermediate buffer is needed.
 * Returns 0 on success, -1 if lwIP rejected the datagram. */
int U2_Net_SendUdp(int i, const uint8_t *ring_base, uint16_t ring_size, uint16_t start_off,
                   uint16_t len, uint32_t dest_ip_net, uint16_t dest_port);
/** IPRAW payload from the TX ring (Sn_PROTO set at OPEN). Returns 0 on success, -1 on failure. */
int U2_Net_SendIpraw(int i, const uint8_t *ring_base, uint16_t ring_size, uint16_t start_off,
                     uint16_t len, uint32_t dest_ip_net);
/** Returns bytes accepted by lwIP (0..len; may be < len under backpressure), or -1 on socket error. */
int U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len);
void U2_Net_RecvConfirm(int i);
/** Host RECV advanced Sn_RX_RD by n bytes. Core 0 opens the TCP window by that much. */
void U2_Net_NoteRecv(int i, uint16_t n);

uint8_t U2_Net_GetStatus(int i);
/** Publish Sn_SR. Call only after the deferred command slot is free. */
void U2_Net_SetStatus(int i, uint8_t status);

/** Drain deferred MACRAW TX (core 0). Safe from `NetworkPump::PollOnce` during native ops. */
void U2_Net_ServicePoll(void);

/** Advance lwIP and drain recv into RX buffers. Call periodically from bus loop. */
void U2_Net_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* _UTHERNET2_NET_H */

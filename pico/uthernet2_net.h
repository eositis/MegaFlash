/**
 * Uthernet II network layer (lwIP TCP/UDP).
 * Call U2_Net_Init() with a push_rx callback; then use U2_Net_* from uthernet2.
 */
#ifndef _UTHERNET2_NET_H
#define _UTHERNET2_NET_H

#include <stdint.h>

/* Callback: push received data into W5100 RX buffer for socket i.
 * is_udp: 1 = UDP (callback will write 4B IP + 2B port + 2B len then payload); 0 = TCP (payload only).
 * src_ip: IPv4 in host order (for UDP header); src_port host order. */
typedef void (*u2_push_rx_fn)(int socket_i, const uint8_t *data, uint16_t len,
                              int is_udp, uint32_t src_ip, uint16_t src_port);
/* Callback: push MACRAW frame into socket i's RX buffer (2-byte length big-endian then frame). */
typedef void (*u2_push_rx_macraw_fn)(int socket_i, const uint8_t *data, uint16_t len);

void U2_Net_Init(u2_push_rx_fn push_rx, u2_push_rx_macraw_fn push_rx_macraw);

void U2_Net_Close(int i);
int  U2_Net_OpenUdp(int i, uint16_t local_port);
int  U2_Net_OpenTcp(int i);
int  U2_Net_OpenMacraw(int i);
void U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len);
/** Feed a received raw Ethernet frame into socket i (MACRAW RX). Call from driver hook if available. */
void U2_Net_FeedMacrawRx(int i, const uint8_t *data, uint16_t len);
int  U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port);
int  U2_Net_ListenTcp(int i, uint16_t local_port);
void U2_Net_SendUdp(int i, const uint8_t *data, uint16_t len, uint32_t dest_ip_net, uint16_t dest_port);
void U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len);
void U2_Net_RecvConfirm(int i);

uint8_t U2_Net_GetStatus(int i);

/** Advance lwIP and drain recv into RX buffers. Call periodically from bus loop. */
void U2_Net_Poll(void);

#endif /* _UTHERNET2_NET_H */

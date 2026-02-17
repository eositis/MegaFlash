/**
 * Uthernet II network layer: lwIP TCP/UDP for W5100 socket emulation.
 * Only built when CYW43/lwIP is available (Pico W).
 */
#include "uthernet2_net.h"
#include "w5100_regs.h"

#if PICO_CYW43_ARCH_POLL

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include <string.h>

#define U2_NET_MAX_SOCKETS  W5100_NUM_SOCKETS
#define U2_MACRAW_MAX_FRAME 1518

static u2_push_rx_fn push_rx_cb;
static u2_push_rx_macraw_fn push_rx_macraw_cb;

typedef enum { PCB_NONE = 0, PCB_UDP, PCB_TCP, PCB_MACRAW } pcb_type_t;

typedef struct {
  union {
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;       /* TCP: client pcb or listen pcb */
  } pcb;
  struct tcp_pcb *tcp_connected; /* TCP: accepted connection (when listening) */
  pcb_type_t type;
  uint8_t    status;  /* W5100_SN_SR_* */
} u2_net_socket_t;

static u2_net_socket_t sockets[U2_NET_MAX_SOCKETS];

static void set_status(int i, uint8_t s) {
  if (i >= 0 && i < U2_NET_MAX_SOCKETS)
    sockets[i].status = s;
}

static uint8_t get_status(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) return W5100_SN_SR_CLOSED;
  return sockets[i].status;
}

/* TCP connected callback */
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
  int i = (int)(intptr_t)arg;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) return ERR_ARG;
  if (err == ERR_OK)
    set_status(i, W5100_SN_SR_ESTABLISHED);
  else
    set_status(i, W5100_SN_SR_CLOSED);
  return ERR_OK;
}

/* TCP recv callback: push payload to W5100 RX buffer */
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
  int i = (int)(intptr_t)arg;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) { if (p) pbuf_free(p); return ERR_ARG; }
  if (err != ERR_OK) { if (p) pbuf_free(p); return err; }
  if (!p) {
    set_status(i, W5100_SN_SR_CLOSED);
    return ERR_OK;
  }
  if (push_rx_cb && p->tot_len > 0) {
    uint8_t *buf = (uint8_t *)p->payload;
    uint16_t len = (uint16_t)p->tot_len;
    push_rx_cb(i, buf, len, 0, 0, 0);
  }
  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

/* TCP accept callback (for listen) */
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
  int i = (int)(intptr_t)arg;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || err != ERR_OK) return ERR_VAL;
  /* Keep listen pcb in .tcp; use .tcp_connected for the connection */
  if (sockets[i].tcp_connected) tcp_close(sockets[i].tcp_connected);
  sockets[i].tcp_connected = newpcb;
  set_status(i, W5100_SN_SR_ESTABLISHED);
  tcp_arg(newpcb, (void *)(intptr_t)i);
  tcp_recv(newpcb, tcp_recv_cb);
  tcp_err(newpcb, NULL);
  return ERR_OK;
}

/* UDP recv callback */
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port) {
  int i = (int)(intptr_t)arg;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || !p) { if (p) pbuf_free(p); return; }
  if (push_rx_cb && p->tot_len > 0) {
    uint32_t ip = ip_addr_get_ip4_u32(addr); /* already in network order on some ports */
    push_rx_cb(i, (const uint8_t *)p->payload, (uint16_t)p->tot_len, 1, ip, port);
  }
  pbuf_free(p);
}

void U2_Net_Init(u2_push_rx_fn push_rx, u2_push_rx_macraw_fn push_rx_macraw) {
  push_rx_cb = push_rx;
  push_rx_macraw_cb = push_rx_macraw;
  memset(sockets, 0, sizeof(sockets));
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++)
    sockets[i].status = W5100_SN_SR_CLOSED;
}

int U2_Net_OpenMacraw(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) return -1;
  U2_Net_Close(i);
  sockets[i].type = PCB_MACRAW;
  sockets[i].status = W5100_SN_SR_SOCK_MACRAW;
  return 0;
}

/* Send raw Ethernet frame via netif linkoutput (full frame including MAC headers). */
void U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !data || len == 0 || len > U2_MACRAW_MAX_FRAME)
    return;
  cyw43_arch_lwip_begin();
  struct netif *netif = netif_list;
  if (netif && netif->linkoutput) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
    if (p) {
      memcpy(p->payload, data, len);
      netif->linkoutput(netif, p);
      pbuf_free(p);
    }
  }
  cyw43_arch_lwip_end();
}

void U2_Net_FeedMacrawRx(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !push_rx_macraw_cb || !data) return;
  push_rx_macraw_cb(i, data, len);
}

void U2_Net_Close(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) return;
  u2_net_socket_t *s = &sockets[i];
  if (s->type == PCB_UDP && s->pcb.udp) {
    udp_remove(s->pcb.udp);
    s->pcb.udp = NULL;
  } else if (s->type == PCB_TCP) {
    if (s->tcp_connected) {
      tcp_arg(s->tcp_connected, NULL);
      tcp_recv(s->tcp_connected, NULL);
      tcp_close(s->tcp_connected);
      s->tcp_connected = NULL;
    }
    if (s->pcb.tcp) {
      tcp_arg(s->pcb.tcp, NULL);
      tcp_accept(s->pcb.tcp, NULL);
      tcp_close(s->pcb.tcp);
      s->pcb.tcp = NULL;
    }
  }
  /* PCB_MACRAW: no pcb to free */
  s->type = PCB_NONE;
  s->status = W5100_SN_SR_CLOSED;
}

int U2_Net_OpenUdp(int i, uint16_t local_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) return -1;
  U2_Net_Close(i);
  cyw43_arch_lwip_begin();
  struct udp_pcb *pcb = udp_new();
  if (!pcb) { cyw43_arch_lwip_end(); return -1; }
  err_t err = udp_bind(pcb, IP4_ADDR_ANY, local_port);
  if (err != ERR_OK) {
    udp_remove(pcb);
    cyw43_arch_lwip_end();
    return -1;
  }
  udp_recv(pcb, udp_recv_cb, (void *)(intptr_t)i);
  sockets[i].pcb.udp = pcb;
  sockets[i].type = PCB_UDP;
  sockets[i].status = W5100_SN_SR_SOCK_UDP;
  cyw43_arch_lwip_end();
  return 0;
}

int U2_Net_OpenTcp(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) return -1;
  U2_Net_Close(i);
  cyw43_arch_lwip_begin();
  struct tcp_pcb *pcb = tcp_new();
  if (!pcb) { cyw43_arch_lwip_end(); return -1; }
  tcp_arg(pcb, (void *)(intptr_t)i);
  sockets[i].pcb.tcp = pcb;
  sockets[i].tcp_connected = NULL;
  sockets[i].type = PCB_TCP;
  sockets[i].status = W5100_SN_SR_SOCK_INIT;
  cyw43_arch_lwip_end();
  return 0;
}

/* Connect TCP: dest_ip_net in network byte order, dest_port in host order */
int U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !sockets[i].pcb.tcp)
    return -1;
  ip_addr_t addr;
  IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF,
           (dest_ip_net >> 8) & 0xFF, dest_ip_net & 0xFF);
  cyw43_arch_lwip_begin();
  err_t err = tcp_connect(sockets[i].pcb.tcp, &addr, dest_port, tcp_connected_cb);
  if (err == ERR_OK)
    set_status(i, W5100_SN_SR_SOCK_SYNSENT);
  cyw43_arch_lwip_end();
  return (err == ERR_OK) ? 0 : -1;
}

int U2_Net_ListenTcp(int i, uint16_t local_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !sockets[i].pcb.tcp)
    return -1;
  cyw43_arch_lwip_begin();
  err_t err = tcp_bind(sockets[i].pcb.tcp, IP4_ADDR_ANY, local_port);
  if (err != ERR_OK) { cyw43_arch_lwip_end(); return -1; }
  struct tcp_pcb *listen = tcp_listen_with_backlog(sockets[i].pcb.tcp, 1);
  if (!listen) { cyw43_arch_lwip_end(); return -1; }
  sockets[i].pcb.tcp = listen;
  tcp_arg(listen, (void *)(intptr_t)i);
  tcp_accept(listen, tcp_accept_cb);
  set_status(i, W5100_SN_SR_SOCK_INIT); /* listen state; W5100 doesn't have exact match, use INIT */
  cyw43_arch_lwip_end();
  return 0;
}

/* Send UDP: dest_ip_net (network order), dest_port (host order) */
void U2_Net_SendUdp(int i, const uint8_t *data, uint16_t len, uint32_t dest_ip_net, uint16_t dest_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_UDP || !sockets[i].pcb.udp || !data)
    return;
  cyw43_arch_lwip_begin();
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
  if (p) {
    memcpy(p->payload, data, len);
    ip_addr_t addr;
    IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF,
             (dest_ip_net >> 8) & 0xFF, dest_ip_net & 0xFF);
    udp_sendto(sockets[i].pcb.udp, p, &addr, dest_port);
    pbuf_free(p);
  }
  cyw43_arch_lwip_end();
}

/* Send TCP: use connected pcb if from accept, else client pcb */
void U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !data || len == 0)
    return;
  struct tcp_pcb *pcb = sockets[i].tcp_connected ? sockets[i].tcp_connected : sockets[i].pcb.tcp;
  if (!pcb) return;
  cyw43_arch_lwip_begin();
  err_t err = tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
  if (err == ERR_OK)
    tcp_output(pcb);
  cyw43_arch_lwip_end();
}

void U2_Net_RecvConfirm(int i) {
  (void)i;
  /* No-op for raw lwIP; RSR is computed from pointers in uthernet2 */
}

uint8_t U2_Net_GetStatus(int i) {
  return get_status(i);
}

void U2_Net_Poll(void) {
  cyw43_arch_lwip_begin();
  cyw43_arch_poll();
  cyw43_arch_lwip_end();
}

#else /* !PICO_CYW43_ARCH_POLL */

static u2_push_rx_fn push_rx_cb;

void U2_Net_Init(u2_push_rx_fn push_rx, u2_push_rx_macraw_fn push_rx_macraw) { (void)push_rx; (void)push_rx_macraw; push_rx_cb = NULL; }
void U2_Net_Close(int i) { (void)i; }
int  U2_Net_OpenUdp(int i, uint16_t local_port) { (void)i; (void)local_port; return -1; }
int  U2_Net_OpenTcp(int i) { (void)i; return -1; }
int  U2_Net_OpenMacraw(int i) { (void)i; return -1; }
void U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len) { (void)i; (void)data; (void)len; }
void U2_Net_FeedMacrawRx(int i, const uint8_t *data, uint16_t len) { (void)i; (void)data; (void)len; }
int  U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port) { (void)i; (void)dest_ip_net; (void)dest_port; return -1; }
int  U2_Net_ListenTcp(int i, uint16_t local_port) { (void)i; (void)local_port; return -1; }
void U2_Net_SendUdp(int i, const uint8_t *data, uint16_t len, uint32_t dest_ip_net, uint16_t dest_port) { (void)i; (void)data; (void)len; (void)dest_ip_net; (void)dest_port; }
void U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len) { (void)i; (void)data; (void)len; }
void U2_Net_RecvConfirm(int i) { (void)i; }
uint8_t U2_Net_GetStatus(int i) { (void)i; return W5100_SN_SR_CLOSED; }
void U2_Net_Poll(void) { }

#endif /* PICO_CYW43_ARCH_POLL */

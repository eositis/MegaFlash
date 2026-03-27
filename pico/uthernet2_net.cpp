/**
 * Uthernet II network layer: lwIP TCP/UDP for W5100 socket emulation.
 * UDP uses NetworkPump::CreateUdpPcb; U2_Net_Poll drives shared NetworkPump_PollOnce.
 * TCP uses per-pcb U2TcpArg (lwIP tcp_err does not pass pcb).
 */
#include "uthernet2_net.h"
#include "uthernet2.h"
#include "u2_monitor.h"
#include "w5100_regs.h"
#include "network.h"
#include "network_pump.h"

#if PICO_CYW43_ARCH_POLL

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include <cstring>

#define U2_NET_MAX_SOCKETS W5100_NUM_SOCKETS
#define U2_MACRAW_MAX_FRAME 1518

typedef enum { PCB_NONE = 0, PCB_UDP, PCB_TCP, PCB_MACRAW } pcb_type_t;

typedef struct {
  union {
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;
  } pcb;
  struct tcp_pcb *tcp_connected;
  pcb_type_t type;
  uint8_t status;
} u2_net_socket_t;

static u2_push_rx_fn push_rx_cb;
static u2_push_rx_macraw_fn push_rx_macraw_cb;
static netif_input_fn u2_saved_netif_input;
static u2_net_socket_t sockets[U2_NET_MAX_SOCKETS];

struct U2TcpArg {
  int sock_index;
};

class Uthernet2Session : public INetworkSession {
public:
  void OnUdpRecvPbuf(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, uint16_t port) override;
  void Abort() override;
};

static Uthernet2Session g_u2_session;

static void set_status(int i, uint8_t s) {
  if (i >= 0 && i < U2_NET_MAX_SOCKETS)
    sockets[i].status = s;
}

static uint8_t get_status(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return W5100_SN_SR_CLOSED;
  return sockets[i].status;
}

static void u2_release_tcp_arg(struct tcp_pcb *pcb) {
  if (!pcb)
    return;
  void *arg = pcb->callback_arg;
  GetNetworkPump().UnregisterTcpPcb(pcb);
  tcp_arg(pcb, nullptr);
  tcp_recv(pcb, nullptr);
  tcp_err(pcb, nullptr);
  tcp_accept(pcb, nullptr);
  delete static_cast<U2TcpArg *>(arg);
}

static void u2_attach_tcp_pcb(struct tcp_pcb *pcb, int i);

extern "C" err_t u2_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
extern "C" void u2_tcp_err(void *arg, err_t err);
extern "C" err_t u2_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err);
extern "C" err_t u2_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);

extern "C" err_t u2_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
  (void)tpcb;
  auto *a = static_cast<U2TcpArg *>(arg);
  if (!a)
    return ERR_ARG;
  int i = a->sock_index;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return ERR_ARG;
  if (err == ERR_OK)
    set_status(i, W5100_SN_SR_ESTABLISHED);
  else
    set_status(i, W5100_SN_SR_CLOSED);
  return ERR_OK;
}

extern "C" err_t u2_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
  auto *a = static_cast<U2TcpArg *>(arg);
  if (!a || !tpcb) {
    if (p)
      pbuf_free(p);
    return ERR_ARG;
  }
  int i = a->sock_index;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS) {
    if (p)
      pbuf_free(p);
    return ERR_ARG;
  }
  if (err != ERR_OK) {
    if (p)
      pbuf_free(p);
    return err;
  }
  if (!p) {
    set_status(i, W5100_SN_SR_CLOSED);
    return ERR_OK;
  }
  if (push_rx_cb && p->tot_len > 0) {
    uint8_t *buf = (uint8_t *)p->payload;
    uint16_t len = (uint16_t)p->tot_len;
    U2_MonNetRxTcp(i, len);
    push_rx_cb(i, buf, len, 0, 0, 0);
  }
  tcp_recved(tpcb, p->tot_len);
  pbuf_free(p);
  return ERR_OK;
}

extern "C" void u2_tcp_err(void *arg, err_t err) {
  (void)err;
  auto *a = static_cast<U2TcpArg *>(arg);
  if (!a)
    return;
  int i = a->sock_index;
  if (i >= 0 && i < U2_NET_MAX_SOCKETS)
    set_status(i, W5100_SN_SR_CLOSED);
}

extern "C" err_t u2_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
  auto *a = static_cast<U2TcpArg *>(arg);
  if (!a || !newpcb)
    return ERR_VAL;
  int i = a->sock_index;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || err != ERR_OK)
    return ERR_VAL;
  if (sockets[i].tcp_connected) {
    u2_release_tcp_arg(sockets[i].tcp_connected);
    tcp_close(sockets[i].tcp_connected);
    sockets[i].tcp_connected = nullptr;
  }
  sockets[i].tcp_connected = newpcb;
  set_status(i, W5100_SN_SR_ESTABLISHED);
  u2_attach_tcp_pcb(newpcb, i);
  return ERR_OK;
}

static void u2_attach_tcp_pcb(struct tcp_pcb *pcb, int i) {
  auto *a = new U2TcpArg{i};
  GetNetworkPump().RegisterTcpPcbOwner(pcb, &g_u2_session);
  tcp_arg(pcb, a);
  tcp_recv(pcb, u2_tcp_recv);
  tcp_err(pcb, u2_tcp_err);
}

void Uthernet2Session::OnUdpRecvPbuf(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, uint16_t port) {
  if (!pcb || !p || !addr)
    return;
  int i = -1;
  for (int j = 0; j < U2_NET_MAX_SOCKETS; j++) {
    if (sockets[j].type == PCB_UDP && sockets[j].pcb.udp == pcb) {
      i = j;
      break;
    }
  }
  if (i < 0)
    return;
  if (push_rx_cb && p->tot_len > 0) {
    uint32_t ip = ip_addr_get_ip4_u32(addr);
    U2_MonNetRxUdp(i, (uint16_t)p->tot_len, ip, port);
    push_rx_cb(i, (const uint8_t *)p->payload, (uint16_t)p->tot_len, 1, ip, port);
  }
}

void Uthernet2Session::Abort() {
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++)
    U2_Net_Close(i);
}

extern "C" {
static err_t u2_netif_input_wrapper(struct pbuf *p, struct netif *inp) {
  if (sockets[0].type == PCB_MACRAW && push_rx_macraw_cb && p && p->tot_len > 0 &&
      p->tot_len <= U2_MACRAW_MAX_FRAME) {
    uint8_t buf[U2_MACRAW_MAX_FRAME];
    u16_t len = (u16_t)pbuf_copy_partial(p, buf, p->tot_len, 0);
    if (len > 0) {
      U2_MonNetRxMacraw(0, len);
      push_rx_macraw_cb(0, buf, len);
    }
  }
  return u2_saved_netif_input(p, inp);
}
} // extern "C"

extern "C" {

void U2_Net_Init(u2_push_rx_fn push_rx, u2_push_rx_macraw_fn push_rx_macraw) {
  push_rx_cb = push_rx;
  push_rx_macraw_cb = push_rx_macraw;
  memset(sockets, 0, sizeof(sockets));
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++)
    sockets[i].status = W5100_SN_SR_CLOSED;
  GetNetworkPump().AddSession(&g_u2_session);
}

int U2_Net_OpenMacraw(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return -1;
  U2_Net_Close(i);
  sockets[i].type = PCB_MACRAW;
  sockets[i].status = W5100_SN_SR_SOCK_MACRAW;
  if (i == 0 && netif_list && netif_list->input && !u2_saved_netif_input) {
    u2_saved_netif_input = netif_list->input;
    netif_list->input = u2_netif_input_wrapper;
  }
  return 0;
}

void U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !data || len == 0 ||
      len > U2_MACRAW_MAX_FRAME)
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
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !push_rx_macraw_cb || !data)
    return;
  push_rx_macraw_cb(i, data, len);
}

void U2_Net_Close(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return;
  if (i == 0 && u2_saved_netif_input && netif_list) {
    netif_list->input = u2_saved_netif_input;
    u2_saved_netif_input = NULL;
  }
  u2_net_socket_t *s = &sockets[i];
  if (s->type == PCB_UDP && s->pcb.udp) {
    GetNetworkPump().DestroyUdpPcb(s->pcb.udp);
    s->pcb.udp = NULL;
  } else if (s->type == PCB_TCP) {
    if (s->tcp_connected) {
      u2_release_tcp_arg(s->tcp_connected);
      tcp_close(s->tcp_connected);
      s->tcp_connected = NULL;
    }
    if (s->pcb.tcp) {
      u2_release_tcp_arg(s->pcb.tcp);
      tcp_close(s->pcb.tcp);
      s->pcb.tcp = NULL;
    }
  }
  s->type = PCB_NONE;
  s->status = W5100_SN_SR_CLOSED;
}

int U2_Net_OpenUdp(int i, uint16_t local_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return -1;
  U2_Net_Close(i);
  cyw43_arch_lwip_begin();
  struct udp_pcb *pcb = GetNetworkPump().CreateUdpPcb(&g_u2_session, local_port);
  cyw43_arch_lwip_end();
  if (!pcb)
    return -1;
  sockets[i].pcb.udp = pcb;
  sockets[i].type = PCB_UDP;
  sockets[i].status = W5100_SN_SR_SOCK_UDP;
  return 0;
}

int U2_Net_OpenTcp(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return -1;
  U2_Net_Close(i);
  cyw43_arch_lwip_begin();
  struct tcp_pcb *pcb = tcp_new();
  if (!pcb) {
    cyw43_arch_lwip_end();
    return -1;
  }
  u2_attach_tcp_pcb(pcb, i);
  sockets[i].pcb.tcp = pcb;
  sockets[i].tcp_connected = NULL;
  sockets[i].type = PCB_TCP;
  sockets[i].status = W5100_SN_SR_SOCK_INIT;
  cyw43_arch_lwip_end();
  return 0;
}

int U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !sockets[i].pcb.tcp)
    return -1;
  ip_addr_t addr;
  IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF, (dest_ip_net >> 8) & 0xFF,
           dest_ip_net & 0xFF);
  cyw43_arch_lwip_begin();
  err_t err = tcp_connect(sockets[i].pcb.tcp, &addr, dest_port, u2_tcp_connected_cb);
  if (err == ERR_OK)
    set_status(i, W5100_SN_SR_SOCK_SYNSENT);
  cyw43_arch_lwip_end();
  return (err == ERR_OK) ? 0 : -1;
}

int U2_Net_ListenTcp(int i, uint16_t local_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !sockets[i].pcb.tcp)
    return -1;
  cyw43_arch_lwip_begin();
  struct tcp_pcb *pcb = sockets[i].pcb.tcp;
  err_t err = tcp_bind(pcb, IP4_ADDR_ANY, local_port);
  if (err != ERR_OK) {
    cyw43_arch_lwip_end();
    return -1;
  }
  u2_release_tcp_arg(pcb);
  struct tcp_pcb *listen = tcp_listen_with_backlog(pcb, 1);
  if (!listen) {
    cyw43_arch_lwip_end();
    return -1;
  }
  sockets[i].pcb.tcp = listen;
  u2_attach_tcp_pcb(listen, i);
  tcp_accept(listen, u2_tcp_accept_cb);
  set_status(i, W5100_SN_SR_SOCK_INIT);
  cyw43_arch_lwip_end();
  return 0;
}

void U2_Net_SendUdp(int i, const uint8_t *data, uint16_t len, uint32_t dest_ip_net, uint16_t dest_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_UDP || !sockets[i].pcb.udp || !data)
    return;
  cyw43_arch_lwip_begin();
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
  if (p) {
    memcpy(p->payload, data, len);
    ip_addr_t addr;
    IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF, (dest_ip_net >> 8) & 0xFF,
             dest_ip_net & 0xFF);
    udp_sendto(sockets[i].pcb.udp, p, &addr, dest_port);
    pbuf_free(p);
  }
  cyw43_arch_lwip_end();
}

void U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !data || len == 0)
    return;
  struct tcp_pcb *pcb = sockets[i].tcp_connected ? sockets[i].tcp_connected : sockets[i].pcb.tcp;
  if (!pcb)
    return;
  cyw43_arch_lwip_begin();
  err_t err = tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
  if (err == ERR_OK)
    tcp_output(pcb);
  cyw43_arch_lwip_end();
}

void U2_Net_RecvConfirm(int i) { (void)i; }

uint8_t U2_Net_GetStatus(int i) { return get_status(i); }

void U2_Net_Poll(void) { NetworkPump_PollOnce(); }

} // extern "C"

#else /* !PICO_CYW43_ARCH_POLL */

extern "C" {

static u2_push_rx_fn push_rx_cb;

void U2_Net_Init(u2_push_rx_fn push_rx, u2_push_rx_macraw_fn push_rx_macraw) {
  (void)push_rx;
  (void)push_rx_macraw;
  push_rx_cb = NULL;
}
void U2_Net_Close(int i) { (void)i; }
int U2_Net_OpenUdp(int i, uint16_t local_port) {
  (void)i;
  (void)local_port;
  return -1;
}
int U2_Net_OpenTcp(int i) {
  (void)i;
  return -1;
}
int U2_Net_OpenMacraw(int i) {
  (void)i;
  return -1;
}
void U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len) {
  (void)i;
  (void)data;
  (void)len;
}
void U2_Net_FeedMacrawRx(int i, const uint8_t *data, uint16_t len) {
  (void)i;
  (void)data;
  (void)len;
}
int U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port) {
  (void)i;
  (void)dest_ip_net;
  (void)dest_port;
  return -1;
}
int U2_Net_ListenTcp(int i, uint16_t local_port) {
  (void)i;
  (void)local_port;
  return -1;
}
void U2_Net_SendUdp(int i, const uint8_t *data, uint16_t len, uint32_t dest_ip_net, uint16_t dest_port) {
  (void)i;
  (void)data;
  (void)len;
  (void)dest_ip_net;
  (void)dest_port;
}
void U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len) {
  (void)i;
  (void)data;
  (void)len;
}
void U2_Net_RecvConfirm(int i) { (void)i; }
uint8_t U2_Net_GetStatus(int i) {
  (void)i;
  return W5100_SN_SR_CLOSED;
}
void U2_Net_Poll(void) {}

} // extern "C"

#endif /* PICO_CYW43_ARCH_POLL */

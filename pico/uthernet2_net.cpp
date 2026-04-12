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
#include "cyw43.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/ip.h"
#include <cstring>
#include <vector>
#if U2_ETH_HEADER_TRACE
#include <cstdio>
#endif

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

/** Always use the CYW43 STA netif (`w0`), not netif_default/netif_list: cyw43_lwip calls
 * netif_set_default() for each netif; if AP (`w1`) is up, default becomes AP and linkoutput
 * would send on the wrong interface. MACRAW RX hook must also attach to STA->input. */
static struct netif *u2_cyw43_sta_netif(void) { return &cyw43_state.netif[CYW43_ITF_STA]; }

#if U2_ETH_HEADER_TRACE
/* UART “poor man’s tcpdump”: first 64 bytes per frame (Eth 14 + IPv4 20 + TCP/UDP start). */
static netif_input_fn u2_eth_trace_saved_input;
static netif_linkoutput_fn u2_eth_trace_saved_linkoutput;
static bool u2_eth_trace_installed;

static void u2_eth_trace_hex_line(const char *dir, const struct netif *nf, const uint8_t *data, size_t got, size_t tot) {
  printf("[u2eth] %s itf=%c%c tot_len=%zu", dir, nf->name[0], nf->name[1], tot);
  for (size_t i = 0; i < got; i++)
    printf(" %02x", data[i]);
  if (tot > got)
    printf(" ...");
  printf("\n");
}

static err_t u2_eth_trace_input(struct pbuf *p, struct netif *inp) {
  if (p && p->tot_len > 0 && inp) {
    uint8_t buf[64];
    u16_t c = pbuf_copy_partial(p, buf, (u16_t)sizeof(buf), 0);
    u2_eth_trace_hex_line("RX", inp, buf, c, p->tot_len);
  }
  return u2_eth_trace_saved_input(p, inp);
}

static err_t u2_eth_trace_linkoutput(struct netif *netif, struct pbuf *p) {
  if (p && p->tot_len > 0 && netif) {
    uint8_t buf[64];
    u16_t c = pbuf_copy_partial(p, buf, (u16_t)sizeof(buf), 0);
    u2_eth_trace_hex_line("TX", netif, buf, c, p->tot_len);
  }
  return u2_eth_trace_saved_linkoutput(netif, p);
}

static void u2_eth_trace_try_install(void) {
  if (u2_eth_trace_installed || !cyw43_is_initialized(&cyw43_state))
    return;
  struct netif *sta = u2_cyw43_sta_netif();
  if (!sta->input || !sta->linkoutput)
    return;
  u2_eth_trace_saved_input = sta->input;
  u2_eth_trace_saved_linkoutput = sta->linkoutput;
  sta->input = u2_eth_trace_input;
  sta->linkoutput = u2_eth_trace_linkoutput;
  u2_eth_trace_installed = true;
}
#endif /* U2_ETH_HEADER_TRACE */

/**
 * ip65 DHCP uses UDP/68→67 inside MACRAW. We overwrite Ethernet SA with netif->hwaddr, but the
 * BOOTP chaddr field (bytes 28–33 of the UDP payload) may still hold a stale WIZnet MAC; DHCP
 * servers often unicast replies using chaddr. Patch chaddr to STA MAC and recompute UDP checksum.
 */
static void u2_macraw_patch_dhcp_bootp_chaddr(uint8_t *eth, uint16_t len, const uint8_t mac[6]) {
  if (len < 14u + 20u + 8u + 29u)
    return;
  uint16_t etype = (uint16_t)(((uint16_t)eth[12] << 8) | eth[13]);
  if (etype != 0x0800)
    return;
  if ((eth[14] >> 4) != 4)
    return;
  uint8_t ihl = (uint8_t)(eth[14] & 0x0Fu) * 4u;
  if (ihl < 20 || (uint32_t)14u + ihl + 8u > len)
    return;
  if (eth[14 + 9] != IP_PROTO_UDP)
    return;
  uint16_t frag = (uint16_t)(((uint16_t)eth[14 + 6] << 8) | eth[14 + 7]);
  if ((frag & 0x3FFFu) != 0)
    return;
  uint16_t ip_total = (uint16_t)(((uint16_t)eth[16] << 8) | eth[17]);
  if (ip_total < ihl || (uint32_t)14u + ip_total > len)
    return;
  uint16_t udp_off = (uint16_t)(14u + ihl);
  uint16_t sport = (uint16_t)(((uint16_t)eth[udp_off] << 8) | eth[udp_off + 1]);
  uint16_t dport = (uint16_t)(((uint16_t)eth[udp_off + 2] << 8) | eth[udp_off + 3]);
  uint16_t udp_len = (uint16_t)(((uint16_t)eth[udp_off + 4] << 8) | eth[udp_off + 5]);
  if (udp_len < 8u + 29u)
    return;
  if ((uint32_t)udp_off + udp_len > len)
    return;
  if ((uint16_t)(ip_total - ihl) != udp_len)
    return;
  if (sport != 68 || dport != 67)
    return;
  uint16_t ulp = (uint16_t)(udp_off + 8);
  if (ulp + 1 >= len)
    return;
  if (eth[ulp] != 1)
    return;
  uint16_t chaddr_off = (uint16_t)(ulp + 28);
  if (chaddr_off + 6 > len)
    return;
  memcpy(eth + chaddr_off, mac, 6);
  eth[udp_off + 6] = 0;
  eth[udp_off + 7] = 0;
  ip4_addr_t src, dst;
  IP4_ADDR(&src, eth[26], eth[27], eth[28], eth[29]);
  IP4_ADDR(&dst, eth[30], eth[31], eth[32], eth[33]);
  struct pbuf *pb = pbuf_alloc(PBUF_RAW, udp_len, PBUF_RAM);
  if (!pb)
    return;
  memcpy(pb->payload, eth + udp_off, udp_len);
  u16_t csum = inet_chksum_pseudo(pb, IP_PROTO_UDP, udp_len, &src, &dst);
  pbuf_free(pb);
  if (csum == 0)
    csum = 0xffff;
  eth[udp_off + 6] = (uint8_t)((csum >> 8) & 0xFF);
  eth[udp_off + 7] = (uint8_t)(csum & 0xFF);
}

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
    uint16_t len = (uint16_t)p->tot_len;
    std::vector<uint8_t> buf(len);
    u16_t copied = pbuf_copy_partial(p, buf.data(), p->tot_len, 0);
    u16_t accepted = 0;
    if (copied > 0) {
      U2_MonNetRxTcp(i, copied);
      accepted = push_rx_cb(i, buf.data(), copied, 0, 0, 0);
    }
    if (accepted > 0) {
      tcp_recved(tpcb, accepted);
    }
  }
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
    uint16_t len = (uint16_t)p->tot_len;
    std::vector<uint8_t> buf(len);
    u16_t copied = pbuf_copy_partial(p, buf.data(), p->tot_len, 0);
    if (copied == 0)
      return;
    uint32_t ip = ip_addr_get_ip4_u32(addr);
    U2_MonNetRxUdp(i, copied, ip, port);
    push_rx_cb(i, buf.data(), copied, 1, ip, port);
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
  /* ip65 DHCP uses MACRAW with Ethernet SA = SHAR. CYW43 transmits frames as-is; APs expect
   * Ethernet source MAC == STA hwaddr. Align SHAR (and TX SA below) with lwIP netif. */
  struct netif *sta = u2_cyw43_sta_netif();
  if (cyw43_is_initialized(&cyw43_state) && sta->hwaddr_len == 6)
    U2_SetStationMacFromBytes(sta->hwaddr);
  if (i == 0 && cyw43_is_initialized(&cyw43_state) && sta->input && !u2_saved_netif_input) {
    u2_saved_netif_input = sta->input;
    sta->input = u2_netif_input_wrapper;
  }
  return 0;
}

void U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !data || len == 0 ||
      len > U2_MACRAW_MAX_FRAME)
    return;
  cyw43_arch_lwip_begin();
  struct netif *netif = u2_cyw43_sta_netif();
  if (cyw43_is_initialized(&cyw43_state) && netif && netif->linkoutput && netif->hwaddr_len == 6) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
    if (p) {
      memcpy(p->payload, data, len);
      uint8_t *eth = (uint8_t *)p->payload;
      /* DHCP: align BOOTP chaddr + UDP checksum with STA MAC (see u2_macraw_patch_dhcp_bootp_chaddr). */
      if (len >= 14 + 20 + 8 + 29)
        u2_macraw_patch_dhcp_bootp_chaddr(eth, len, netif->hwaddr);
      /* Ethernet header: bytes 0–5 DA, 6–11 SA. Force SA = STA MAC so frames are not dropped
       * when ip65 built the header from a different SHAR than CYW43. */
      if (len >= 12)
        memcpy(eth + 6, netif->hwaddr, 6);
      U2_SetStationMacFromBytes(netif->hwaddr);
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
  if (i == 0 && u2_saved_netif_input && cyw43_is_initialized(&cyw43_state)) {
    u2_cyw43_sta_netif()->input = u2_saved_netif_input;
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
  struct udp_pcb *pcb = GetNetworkPump().CreateUdpPcb(&g_u2_session, local_port);
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

int U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port, uint16_t local_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !sockets[i].pcb.tcp)
    return -1;
  ip_addr_t addr;
  IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF, (dest_ip_net >> 8) & 0xFF,
           dest_ip_net & 0xFF);
  cyw43_arch_lwip_begin();
  if (local_port != 0) {
    err_t br = tcp_bind(sockets[i].pcb.tcp, IP4_ADDR_ANY, local_port);
    if (br != ERR_OK) {
      cyw43_arch_lwip_end();
      return -1;
    }
  }
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

void U2_Net_Poll(void) {
#if U2_ETH_HEADER_TRACE
  u2_eth_trace_try_install();
#endif
  NetworkPump_PollOnce();
}

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
int U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port, uint16_t local_port) {
  (void)i;
  (void)dest_ip_net;
  (void)dest_port;
  (void)local_port;
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

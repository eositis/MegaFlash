/**
 * Uthernet II network layer: lwIP TCP/UDP for W5100 socket emulation.
 * UDP uses NetworkPump::CreateUdpPcb; U2_Net_Poll drives shared NetworkPump_PollOnce.
 * TCP uses per-pcb U2TcpArg (lwIP tcp_err does not pass pcb).
 */
#define U2_FILE_ID 1
#include "u2_lockdbg.h"
#include "uthernet2_net.h"
#include "uthernet2.h"
#include "u2_monitor.h"
#include "u2_busaudit.h"
#include "u2_busram.h"
#include "w5100_regs.h"
#include "network.h"
#include "network_pump.h"

#if PICO_CYW43_ARCH_POLL

#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/ip.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "a2bus.h"
#include <cstring>
#include <cstdio>
#include <vector>
#if U2_ETH_HEADER_TRACE
#include <cstdio>
#endif

#define U2_NET_MAX_SOCKETS W5100_NUM_SOCKETS
#define U2_MACRAW_MAX_FRAME 1518
#if PICO_RP2040
#define U2_MACRAW_TX_Q_DEPTH 4
#else
#define U2_MACRAW_TX_Q_DEPTH 16
#endif
#define U2_MACRAW_TX_DRAIN_PER_POLL 8

typedef enum { PCB_NONE = 0, PCB_UDP, PCB_TCP, PCB_MACRAW, PCB_IPRAW } pcb_type_t;

typedef struct {
  union {
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;
  } pcb;
  struct tcp_pcb *tcp_connected;
  struct raw_pcb *raw;
  struct tcp_pcb *pend_tcp;
  struct tcp_pcb *pend_conn;
  struct udp_pcb *pend_udp;
  struct raw_pcb *pend_raw;
  pcb_type_t type;
  uint8_t status;
  uint8_t nd;            /* Sn_MR ND: tcp_nagle_disable */
  uint8_t tx_fin_sent;   /* host DISCON has shut the TX side */
  uint8_t silent_abort;  /* CLOSE / reset: tcp_err must not raise TIMEOUT */
  uint8_t need_close;    /* core 1 asked core 0 to free the pcb */
  uint8_t retire_listen; /* accepted one child; drop the listen pcb */
  uint8_t born_epoch;    /* U2_CmdEpoch() when this pcb was opened. Reset closes older ones. */
  uint8_t hold_noted;   /* one UART line while a hold cannot enter the ring */
  struct pbuf *hold;     /* TCP bytes ACKed but not yet in the W5100 ring */
} u2_net_socket_t;

static u2_push_rx_fn push_rx_cb;
static u2_push_rx_macraw_fn push_rx_macraw_cb;
static netif_input_fn u2_saved_netif_input;
/* First real STA input. u2_saved_netif_input is cleared once MACRAW ends, and
 * a second hook install then has nothing to put back. */
static netif_input_fn u2_sta_input_real;
/* Core 1 asks core 0 to install (1) or restore (0) sta->input. Never written from the bus loop
 * under the lwIP lock: a core-1 cyw43_arch_lwip_begin stalls the PIO prefetch. */
static uint8_t u2_macraw_hook;
static volatile uint8_t u2_net_reset_pending;
static u2_net_socket_t sockets[U2_NET_MAX_SOCKETS];
/* Plain spin lock (inline SDK primitives only) instead of critical_section_t: the bus core takes
 * this lock from SRAM, and critical_section_enter_blocking is flash code (section 1ev). */
static spin_lock_t *u2_macraw_tx_lock;
static inline __attribute__((always_inline)) uint32_t u2_tx_lock(void) {
  return spin_lock_blocking(u2_macraw_tx_lock);
}
static inline __attribute__((always_inline)) void u2_tx_unlock(uint32_t s) {
  spin_unlock(u2_macraw_tx_lock, s);
}

typedef struct {
  int sock;
  uint16_t len;
  uint8_t data[U2_MACRAW_MAX_FRAME];
} u2_macraw_tx_slot_t;

static u2_macraw_tx_slot_t u2_macraw_tx_q[U2_MACRAW_TX_Q_DEPTH];
static uint8_t u2_macraw_tx_q_head;
static uint8_t u2_macraw_tx_q_count;
static uint32_t u2_macraw_tx_q_drop;
static uint32_t u2_macraw_tx_lo_err;
static uint32_t u2_macraw_tx_pbuf_fail;
static absolute_time_t u2_macraw_tx_stats_next;

struct U2TcpArg {
  int sock_index;
  struct tcp_pcb *pcb;
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
static inline __attribute__((always_inline)) struct netif *u2_cyw43_sta_netif(void) { return &cyw43_state.netif[CYW43_ITF_STA]; }

#if U2_ETH_HEADER_TRACE
/* UART “poor man’s tcpdump”: first 64 bytes per frame (Eth 14 + IPv4 20 + TCP/UDP start). */
static netif_input_fn u2_eth_trace_saved_input;
static netif_linkoutput_fn u2_eth_trace_saved_linkoutput;
static bool u2_eth_trace_installed;

/* Extra TX tap for MACRAW triage: compare bytes at queue-drain vs pre/post patch. */
static void u2_eth_trace_tap(const char *stage, const uint8_t *data, uint16_t len) {
  if (!data || len == 0)
    return;
  /* Keep noise bounded: focus on DHCP-sized/nearby MACRAW sends. */
  if (len < 200 && len != 257 && len != 292)
    return;
  size_t got = len < 64 ? len : 64;
  printf("[u2tap] %s len=%u", stage, (unsigned)len);
  for (size_t i = 0; i < got; i++)
    printf(" %02x", data[i]);
  if (len > got)
    printf(" ...");
  printf("\n");
}

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
 * BOOTP chaddr field (bytes 28–33 of the UDP payload) and DHCP client-id option (61) may still
 * hold a stale MAC. Patch both to STA MAC. For IPv4 UDP, checksum 0 means "no checksum"; we use
 * that here to avoid interoperability issues from on-device checksum rewrite mistakes.
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
  /* DHCP options start at BOOTP offset 240: fixed 236-byte header + 4-byte magic cookie. */
  uint16_t opts = (uint16_t)(ulp + 240u);
  uint16_t end = (uint16_t)(ulp + udp_len - 8u);
  if (opts < end && opts + 4u <= end) {
    /* Validate magic cookie 99,130,83,99 and patch option 61 (client-id, type 1 + MAC). */
    if (eth[opts + 0] == 99 && eth[opts + 1] == 130 && eth[opts + 2] == 83 && eth[opts + 3] == 99) {
      uint16_t p = (uint16_t)(opts + 4u);
      while (p < end) {
        uint8_t code = eth[p++];
        if (code == 0) /* pad */
          continue;
        if (code == 255) /* end */
          break;
        if (p >= end)
          break;
        uint8_t olen = eth[p++];
        if (p + olen > end)
          break;
        if (code == 61 && olen >= 7 && eth[p] == 1)
          memcpy(eth + p + 1, mac, 6);
        p = (uint16_t)(p + olen);
      }
    }
  }
  /* IPv4 UDP checksum 0 is valid (checksum disabled). */
  eth[udp_off + 6] = 0;
  eth[udp_off + 7] = 0;
}

/* Host 1026 is Contiki/ip65's first ephemeral. A *fixed* wire port (41226) becomes the same
 * 4-tuple after every reboot (same STA IP), so a prior session's TIME-WAIT black-holes the
 * first SYN; 1027/1028 are new 4-tuples and SYN-ACK in ~25 ms (§1eb UART). Pick a fresh
 * ephemeral on each SYN; later 1026 segments use that mapping until the next SYN or CLOSE. */
#define U2_PNAT_HOST 1026u
#define U2_PNAT_WIRE_BASE 49152u
#define U2_PNAT_WIRE_SPAN 8191u
#define U2_PNAT_RX_KEEP 8
static uint8_t u2_pnat_used;
static uint16_t u2_pnat_wire;
static uint16_t u2_pnat_last;
static uint16_t u2_pnat_rx_ports[U2_PNAT_RX_KEEP];
static uint8_t u2_pnat_rx_n;

static uint16_t u2_pnat_pick_wire(void) {
  uint16_t p = (uint16_t)(U2_PNAT_WIRE_BASE + (time_us_32() % U2_PNAT_WIRE_SPAN));
  if (p == u2_pnat_last)
    p = (uint16_t)(U2_PNAT_WIRE_BASE + ((p + 1u - U2_PNAT_WIRE_BASE) % U2_PNAT_WIRE_SPAN));
  u2_pnat_last = p;
  u2_pnat_rx_ports[u2_pnat_rx_n % U2_PNAT_RX_KEEP] = p;
  u2_pnat_rx_n++;
  return p;
}

static int u2_pnat_rx_match(uint16_t dport) {
  uint8_t n = u2_pnat_rx_n < U2_PNAT_RX_KEEP ? u2_pnat_rx_n : U2_PNAT_RX_KEEP;
  for (uint8_t i = 0; i < n; i++) {
    if (u2_pnat_rx_ports[i] == dport)
      return 1;
  }
  return 0;
}

static void U2_BUS_RAM(u2_pnat_reset)(void) {
  u2_pnat_used = 0;
  u2_pnat_wire = 0;
  u2_pnat_rx_n = 0;
}

static void u2_tcp_csum_replace2(uint8_t *tcp, uint16_t oldv, uint16_t newv) {
  uint32_t hc = ((uint32_t)tcp[16] << 8) | tcp[17];
  uint32_t x = (~hc & 0xffffu) + (~(uint32_t)oldv & 0xffffu) + newv;
  while (x >> 16)
    x = (x & 0xffffu) + (x >> 16);
  uint16_t hc2 = (uint16_t)~x;
  if (hc2 == 0)
    hc2 = 0xffff;
  tcp[16] = (uint8_t)(hc2 >> 8);
  tcp[17] = (uint8_t)hc2;
}

static int u2_tcp_hdr(uint8_t *eth, uint16_t len, uint8_t **tcp_out) {
  if (!eth || len < 40 || eth[12] != 0x08 || eth[13] != 0x00 || eth[23] != 6)
    return 0;
  uint8_t ihl = (uint8_t)((eth[14] & 0x0Fu) * 4u);
  uint16_t off = (uint16_t)(14u + ihl);
  if (len < (uint16_t)(off + 20u))
    return 0;
  *tcp_out = eth + off;
  return 1;
}

static void u2_pnat_tx(uint8_t *eth, uint16_t len) {
  uint8_t *tcp;
  if (!u2_tcp_hdr(eth, len, &tcp))
    return;
  uint16_t sport = (uint16_t)(((uint16_t)tcp[0] << 8) | tcp[1]);
  if ((tcp[13] & 0x12u) == 0x02u && sport == U2_PNAT_HOST) {
    u2_pnat_wire = u2_pnat_pick_wire();
    u2_pnat_used = 1;
  }
  if (u2_pnat_used && u2_pnat_wire && sport == U2_PNAT_HOST) {
    tcp[0] = (uint8_t)(u2_pnat_wire >> 8);
    tcp[1] = (uint8_t)u2_pnat_wire;
    u2_tcp_csum_replace2(tcp, U2_PNAT_HOST, u2_pnat_wire);
  }
}

static void u2_pnat_rx(uint8_t *eth, uint16_t len) {
  uint8_t *tcp;
  if (!u2_pnat_used || !u2_tcp_hdr(eth, len, &tcp))
    return;
  uint16_t dport = (uint16_t)(((uint16_t)tcp[2] << 8) | tcp[3]);
  if (!u2_pnat_rx_match(dport))
    return;
  tcp[2] = (uint8_t)(U2_PNAT_HOST >> 8);
  tcp[3] = (uint8_t)U2_PNAT_HOST;
  u2_tcp_csum_replace2(tcp, dport, U2_PNAT_HOST);
}

static void U2_BUS_RAM(set_status)(int i, uint8_t s) {
  if (i >= 0 && i < U2_NET_MAX_SOCKETS) {
    U2_BaTrace("st", i, s, sockets[i].status);
    __atomic_store_n(&sockets[i].status, s, __ATOMIC_RELEASE);
    U2_SrPublish(i, s);
  }
}

static uint8_t U2_BUS_RAM(get_status)(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return W5100_SN_SR_CLOSED;
  return __atomic_load_n(&sockets[i].status, __ATOMIC_ACQUIRE);
}

static int U2_BUS_RAM(u2_need_close)(int i) {
  return __atomic_load_n(&sockets[i].need_close, __ATOMIC_ACQUIRE) != 0;
}

static void u2_release_tcp_arg(struct tcp_pcb *pcb) {
  if (!pcb)
    return;
  void *arg = pcb->callback_arg;
  GetNetworkPump().UnregisterTcpPcb(pcb);
  tcp_arg(pcb, nullptr);
  /* lwIP asserts (LWIP_ASSERT -> panic) if tcp_recv/tcp_err/tcp_sent/tcp_poll is called on a
   * LISTEN pcb. A listener only has arg and accept. */
  if (pcb->state != LISTEN) {
    tcp_recv(pcb, nullptr);
    tcp_err(pcb, nullptr);
  }
  tcp_accept(pcb, nullptr);
  delete static_cast<U2TcpArg *>(arg);
}

/* tcp_abort() asserts on a LISTEN pcb (panic); a listener is released with tcp_close(). */
static void u2_tcp_kill(struct tcp_pcb *pcb) {
  if (pcb->state == LISTEN)
    tcp_close(pcb);
  else
    tcp_abort(pcb);
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
  if (u2_need_close(i))
    return ERR_ABRT;
  /* A SYN-ACK for a pcb this socket no longer owns must not rewrite Sn_SR. */
  if (tpcb && sockets[i].pcb.tcp != tpcb && sockets[i].tcp_connected != tpcb)
    return ERR_OK;
  if (err == ERR_OK) {
    set_status(i, W5100_SN_SR_ESTABLISHED);
    if (sockets[i].nd)
      tcp_nagle_disable(tpcb);
    U2_SocketIrq(i, W5100_SN_IR_CON);
    // #region agent log
    U2_AgentLog("H3", "con", i, W5100_SN_SR_ESTABLISHED, 0, 1);
    // #endregion
  } else {
    set_status(i, W5100_SN_SR_CLOSED);
    U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
    // #region agent log
    U2_AgentLog("H3", "con-fail", i, (int)err, 0, 1);
    // #endregion
  }
  return ERR_OK;
}

static struct tcp_pcb *u2_tcp_pcb(int i) {
  if (sockets[i].tcp_connected)
    return sockets[i].tcp_connected;
  return sockets[i].pcb.tcp;
}

/* Bytes copied into the W5100 ring. Compared with Sn_RX_RD + Sn_RX_RSR at the
 * heartbeat: a gap means the ring lost or duplicated payload. */
static volatile uint32_t u2_dbg_push_n;

/* Move as much of `p` as the W5100 ring will take. The remainder stays on the
 * socket. Caller already owns `p` (this does not return it to lwIP). */
static void u2_flush_hold(int i) {
  struct pbuf *p = sockets[i].hold;
  struct tcp_pcb *tpcb = u2_tcp_pcb(i);
  sockets[i].hold = nullptr;
  if (!p)
    return;
  if (!tpcb || !push_rx_cb) {
    pbuf_free(p);
    return;
  }
  uint16_t pushed = 0;
  uint16_t before = p->tot_len;
  while (p && p->tot_len > 0) {
    if (p->len == 0) {
      struct pbuf *n = p->next;
      p->next = nullptr;
      pbuf_free(p);
      p = n;
      continue;
    }
    u16_t chunk = p->len;
    u16_t acc = push_rx_cb(i, static_cast<const uint8_t *>(p->payload), chunk, 0, 0, 0);
    if (acc == 0)
      break;
    /* Window stays closed until the host RECV. tcp_recved here reopened it
     * while the bytes were still unread, so the peer filled the 8KB ring and
     * kept sending. */
    (void)tpcb;
    pushed = (uint16_t)(pushed + acc);
    u2_dbg_push_n += acc;
    p = pbuf_free_header(p, acc);
    if (acc < chunk)
      break;
  }
  sockets[i].hold = p;
  // #region agent log
  if (pushed > 0) {
    sockets[i].hold_noted = 0;
    U2_AgentLog("H1", "rx", i, (int)before, (int)pushed, 0);
  } else if (p && !sockets[i].hold_noted) {
    sockets[i].hold_noted = 1;
    U2_AgentLog("H1", "rx-hold", i, (int)before, 0, 1);
    // #region agent log
    {
      int sz = 0, live = 0, sh = 0;
      U2_RxDebugStat(i, &sz, &live, &sh);
      U2_AgentLog("H9", "rx-stat", sz, live, sh, 1);
    }
    // #endregion
  }
  // #endregion
}

static err_t u2_tcp_hold_rx(int i, struct tcp_pcb *tpcb, struct pbuf *p) {
  (void)tpcb;
  u2_flush_hold(i);
  if (!p)
    return ERR_OK;
  if (sockets[i].hold) {
    /* One held remainder is enough. Another segment would pin every in-flight
     * Wi-Fi pbuf (the chain passed 11KB and the chip died during cover art).
     * Leave this pbuf with lwIP; the window stays closed until the hold drains. */
    // #region agent log
    U2_AgentLog("H1", "rx-block", i, (int)p->tot_len, (int)sockets[i].hold->tot_len, 1);
    // #endregion
    return ERR_MEM;
  }
  sockets[i].hold = p;
  u2_flush_hold(i);
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
  if (u2_need_close(i)) {
    if (p)
      pbuf_free(p);
    return ERR_OK;
  }
  if (err != ERR_OK) {
    if (p)
      pbuf_free(p);
    return err;
  }
  if (!p) {
    /* Peer FIN. If we already shut TX (DISCON), reap the pcb and go CLOSED.
     * Otherwise stay in CLOSE_WAIT so the host can still RECV, then DISCON. */
    // #region agent log
    U2_AgentLog("H4", "peer-fin", i, sockets[i].tx_fin_sent, get_status(i), 1);
    // #endregion
    if (sockets[i].tx_fin_sent) {
      set_status(i, W5100_SN_SR_CLOSED);
      U2_SocketIrq(i, W5100_SN_IR_DISCON);
      sockets[i].silent_abort = 1;
      if (sockets[i].tcp_connected == tpcb)
        sockets[i].tcp_connected = nullptr;
      if (sockets[i].pcb.tcp == tpcb)
        sockets[i].pcb.tcp = nullptr;
      sockets[i].tx_fin_sent = 0;
      tcp_abort(tpcb);
      return ERR_ABRT;
    }
    set_status(i, W5100_SN_SR_SOCK_CLOSE_WAIT);
    U2_SocketIrq(i, W5100_SN_IR_DISCON);
    return ERR_OK;
  }
  if (p->tot_len > 0) {
    /* Datasheet Sn_MR bit 5 (ND): ACK as soon as a data segment is received.
     * tcp_nagle_disable only changes our sends. tcp_output follows this callback. */
    if (sockets[i].nd)
      tcp_set_flags(tpcb, TF_ACK_NOW);
    return u2_tcp_hold_rx(i, tpcb, p);
  }
  pbuf_free(p);
  return ERR_OK;
}

extern "C" void u2_tcp_err(void *arg, err_t err) {
  auto *a = static_cast<U2TcpArg *>(arg);
  if (!a)
    return;
  int i = a->sock_index;
  struct tcp_pcb *dead = a->pcb;
  delete a;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return;
  int owned = 0;
  if (dead) {
    GetNetworkPump().UnregisterTcpPcb(dead);
    if (sockets[i].tcp_connected == dead) {
      sockets[i].tcp_connected = nullptr;
      owned = 1;
    }
    if (sockets[i].pcb.tcp == dead) {
      sockets[i].pcb.tcp = nullptr;
      owned = 1;
    }
  } else {
    owned = 1;
  }
  /* The slot may already hold the next test's pcb. Closing that status is the $00 row. */
  if (!owned && (sockets[i].pcb.tcp || sockets[i].tcp_connected))
    return;
  uint8_t silent = sockets[i].silent_abort;
  uint8_t fin = sockets[i].tx_fin_sent;
  sockets[i].silent_abort = 0;
  sockets[i].tx_fin_sent = 0;
  // #region agent log
  U2_AgentLog("H3", "tcp-err", i, (int)err, silent ? 1 : (fin ? 2 : 0), 1);
  // #endregion
  set_status(i, W5100_SN_SR_CLOSED);
  if (sockets[i].pcb.tcp == nullptr && sockets[i].tcp_connected == nullptr &&
      sockets[i].type == PCB_TCP)
    sockets[i].type = PCB_NONE;
  if (silent)
    return;
  if (fin || err == ERR_CLSD)
    U2_SocketIrq(i, W5100_SN_IR_DISCON);
  else
    U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
}

extern "C" err_t u2_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
  auto *a = static_cast<U2TcpArg *>(arg);
  U2_BaTrace("accept", a ? a->sock_index : -1, (int32_t)err, newpcb ? (int32_t)newpcb->remote_port : 0);
  if (!a || !newpcb)
    return ERR_VAL;
  int i = a->sock_index;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || err != ERR_OK)
    return ERR_VAL;
  /* W5100 listen becomes the one accepted connection. A second SYN is refused. */
  if (sockets[i].tcp_connected || u2_need_close(i)) {
    tcp_abort(newpcb);
    return ERR_ABRT;
  }
  sockets[i].tcp_connected = newpcb;
  set_status(i, W5100_SN_SR_ESTABLISHED);
  u2_attach_tcp_pcb(newpcb, i);
  if (sockets[i].nd)
    tcp_nagle_disable(newpcb);
  U2_SocketIrq(i, W5100_SN_IR_CON);
  sockets[i].retire_listen = 1;
  return ERR_OK;
}

static void u2_attach_tcp_pcb(struct tcp_pcb *pcb, int i) {
  auto *a = new U2TcpArg{i, pcb};
  GetNetworkPump().RegisterTcpPcbOwner(pcb, &g_u2_session);
  tcp_arg(pcb, a);
  /* b151 audit: core 0 died here for every LISTEN. lwIP's tcp_recv()/tcp_err() assert
   * "invalid socket state for recv/err callback" when pcb->state == LISTEN, and LWIP_ASSERT is
   * panic() on this build. A listener needs only arg (and accept, set by the caller). */
  if (pcb->state == LISTEN)
    return;
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
  U2_BaTrace(i < 0 ? "udp_rx_nosock" : "udp_rx", i, (int32_t)p->tot_len, (int32_t)port);
  if (i < 0 || u2_need_close(i))
    return;
  if (push_rx_cb && p->tot_len > 0) {
    uint16_t len = (uint16_t)p->tot_len;
    std::vector<uint8_t> buf(len);
    u16_t copied = pbuf_copy_partial(p, buf.data(), p->tot_len, 0);
    if (copied == 0)
      return;
    uint32_t ip = ((uint32_t)ip4_addr1(ip_2_ip4(addr)) << 24) |
                  ((uint32_t)ip4_addr2(ip_2_ip4(addr)) << 16) |
                  ((uint32_t)ip4_addr3(ip_2_ip4(addr)) << 8) |
                  (uint32_t)ip4_addr4(ip_2_ip4(addr));
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
  if (!p)
    return ERR_ARG;

  const bool legacy = GetNetworkPump().IsLegacyOperationActive();
  const bool macraw0 = (sockets[0].type == PCB_MACRAW && push_rx_macraw_cb);

  /* Native MegaFlash (NTP/TFTP/TestWifi): lwIP owns STA ingress; do not copy into ip65 ring.
   * Only while the Apple's MACRAW socket is closed (section 1ev). An NTP attempt that was already
   * running (or a Wi-Fi retry) used to take every frame for its whole duration, so a MACRAW OPEN
   * during it received no DHCP/ARP replies at all: the same build then passed or failed depending
   * on boot-time NTP luck. With MACRAW open the Apple wins and the native op just times out. */
  if (legacy && !macraw0) {
    if (u2_saved_netif_input)
      return u2_saved_netif_input(p, inp);
    pbuf_free(p);
    return ERR_ARG;
  }

  /* ip65 / Contiki MACRAW: deliver to W5100 ring only; lwIP must not see TCP/UDP (spurious RST). */
  if (macraw0) {
    if (p->tot_len > 0 && p->tot_len <= U2_MACRAW_MAX_FRAME) {
      uint8_t buf[U2_MACRAW_MAX_FRAME];
      u16_t len = (u16_t)pbuf_copy_partial(p, buf, p->tot_len, 0);
      if (len > 0) {
        u2_pnat_rx(buf, len);
        U2_MonNetRxMacraw(0, len);
        push_rx_macraw_cb(0, buf, len);
      }
    }
    pbuf_free(p);
    return ERR_OK;
  }

  if (u2_saved_netif_input)
    return u2_saved_netif_input(p, inp);
  if (u2_sta_input_real)
    return u2_sta_input_real(p, inp);
  pbuf_free(p);
  return ERR_ARG;
}

} // extern "C"

extern "C" {

static bool u2_send_macraw_core0(int i, const uint8_t *data, uint16_t len);

static void U2_BUS_RAM(u2_macraw_tx_queue_clear)(void) {
  uint32_t sv = u2_tx_lock();
  u2_macraw_tx_q_head = 0;
  u2_macraw_tx_q_count = 0;
  u2_tx_unlock(sv);
}

/* SRAM-only copy (newlib memcpy is flash). Unaligned 32-bit accesses are legal on Cortex-M33. */
typedef uint32_t __attribute__((may_alias, aligned(1))) u2_u32u_t;
static inline __attribute__((always_inline)) void u2_ram_copy(uint8_t *d, const uint8_t *s, uint32_t n) {
  while (n >= 4u) {
    *(u2_u32u_t *)d = *(const u2_u32u_t *)s;
    d += 4;
    s += 4;
    n -= 4u;
  }
  while (n--)
    *d++ = *s++;
}

/* Bus core: ring (power-of-two size mask+1, wrapping) -> queue slot, one copy, SRAM only. */
int U2_BUS_RAM(U2_Net_SendMacrawRing)(int i, const uint8_t *ring, uint16_t mask, uint16_t start,
                                      uint16_t len) {
  if ((unsigned)i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !ring || len == 0 ||
      len > U2_MACRAW_MAX_FRAME)
    return -1;
  uint32_t sv = u2_tx_lock();
  if (u2_macraw_tx_q_count >= U2_MACRAW_TX_Q_DEPTH) {
    u2_macraw_tx_q_drop++;
    u2_tx_unlock(sv);
    return -1;
  }
  uint8_t tail = (uint8_t)((u2_macraw_tx_q_head + u2_macraw_tx_q_count) % U2_MACRAW_TX_Q_DEPTH);
  u2_macraw_tx_q[tail].sock = i;
  u2_macraw_tx_q[tail].len = len;
  uint8_t *dst = u2_macraw_tx_q[tail].data;
  uint16_t off = (uint16_t)(start & mask);
  uint32_t first = (uint32_t)mask + 1u - off;
  if (first >= len) {
    u2_ram_copy(dst, ring + off, len);
  } else {
    u2_ram_copy(dst, ring + off, first);
    u2_ram_copy(dst + first, ring, (uint32_t)(len - first));
  }
  u2_macraw_tx_q_count++;
  u2_tx_unlock(sv);
  return 0;
}

static void u2_macraw_tx_drain(void) {
  for (int n = 0; n < U2_MACRAW_TX_DRAIN_PER_POLL; n++) {
    int sock;
    uint16_t len;
    uint8_t buf[U2_MACRAW_MAX_FRAME];
    uint32_t sv = u2_tx_lock();
    if (u2_macraw_tx_q_count == 0) {
      u2_tx_unlock(sv);
      break;
    }
    sock = u2_macraw_tx_q[u2_macraw_tx_q_head].sock;
    len = u2_macraw_tx_q[u2_macraw_tx_q_head].len;
    memcpy(buf, u2_macraw_tx_q[u2_macraw_tx_q_head].data, len);
    /* Peek only: pop after linkoutput succeeds so a failed send retries the same frame.
     * Do not emit twice — one copy, one drain attempt per poll slot. */
    u2_tx_unlock(sv);

    U2_LWIP_BEGIN();
    bool ok = u2_send_macraw_core0(sock, buf, len);
    U2_LWIP_END();

    if (!ok)
      break;

    uint32_t sv2 = u2_tx_lock();
    u2_macraw_tx_q_head = (uint8_t)((u2_macraw_tx_q_head + 1) % U2_MACRAW_TX_Q_DEPTH);
    u2_macraw_tx_q_count--;
    u2_tx_unlock(sv2);
  }
}

static bool u2_send_macraw_core0(int i, const uint8_t *data, uint16_t len) {
  struct netif *netif = u2_cyw43_sta_netif();
  if (!cyw43_is_initialized(&cyw43_state) || !netif || !netif->linkoutput || netif->hwaddr_len != 6) {
    return false;
  }
  /* W5100 MAC pads TX to 60 bytes (excl. FCS). CYW43 sends the pbuf length as-is, so 54–58
   * byte SYNs leave as Ethernet runts unless we pad here. */
  const uint16_t send_len = (len < 60u) ? 60u : len;
  struct pbuf *p = pbuf_alloc(PBUF_RAW, send_len, PBUF_RAM);
  if (!p) {
    u2_macraw_tx_pbuf_fail++;
    return false;
  }
  memcpy(p->payload, data, len);
  if (send_len > len)
    memset((uint8_t *)p->payload + len, 0, (size_t)(send_len - len));
  uint8_t *eth = (uint8_t *)p->payload;
#if U2_ETH_HEADER_TRACE
  u2_eth_trace_tap("core0-pre", eth, send_len);
#endif
  if (send_len >= 12)
    memcpy(eth + 6, netif->hwaddr, 6);
  /* ARP sender-hardware-address normalization: ip65's own MAC is the WIZnet OUI (00:08:DC:..),
   * so an ARP request/reply carries that inside the payload even though we rewrite the Ethernet
   * SA to the STA MAC. The gateway then learns our IP against the WIZnet MAC and unicasts the ARP
   * reply to a MAC the AP never delivers to this station -> host stuck in ARP retry, DNS never
   * proceeds (see debug/megaflash-2.pcap; §1aa/§1ad). Point the ARP SHA at the STA MAC too.
   * ethertype 0x0806 at eth[12:13]; ARP SHA is at payload offset 8 = frame offset 22. */
  if (len >= 28 && eth[12] == 0x08 && eth[13] == 0x06)
    memcpy(eth + 22, netif->hwaddr, 6);
  u2_macraw_patch_dhcp_bootp_chaddr(eth, len, netif->hwaddr);
  u2_pnat_tx(eth, len);
#if U2_ETH_HEADER_TRACE
  u2_eth_trace_tap("core0-post", eth, send_len);
#endif
  U2_SetStationMacFromBytes(netif->hwaddr);
  err_t err = netif->linkoutput(netif, p);
  pbuf_free(p);
  if (err != ERR_OK) {
    u2_macraw_tx_lo_err++;
    return false;
  }
  U2_MonNetMacrawTx(i, len);
  return true;
}

void U2_Net_Init(u2_push_rx_fn push_rx, u2_push_rx_macraw_fn push_rx_macraw) {
  push_rx_cb = push_rx;
  push_rx_macraw_cb = push_rx_macraw;
  memset(sockets, 0, sizeof(sockets));
  if (!u2_macraw_tx_lock)
    u2_macraw_tx_lock = spin_lock_init((uint)spin_lock_claim_unused(true));
  u2_macraw_tx_queue_clear();
  u2_macraw_tx_q_drop = 0;
  u2_macraw_tx_lo_err = 0;
  u2_macraw_tx_pbuf_fail = 0;
  u2_macraw_tx_stats_next = make_timeout_time_ms(10000);
  u2_macraw_hook = 0;
  u2_saved_netif_input = NULL;
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++)
    set_status(i, W5100_SN_SR_CLOSED);
  GetNetworkPump().AddSession(&g_u2_session);
}

int U2_BUS_RAM(U2_Net_OpenMacraw)(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return -1;
  U2_Net_CloseBus(i);
  /* A TCP/UDP/IPRAW pcb is still core 0's. Overwriting the union here would leak it. */
  if (sockets[i].type != PCB_NONE || u2_need_close(i))
    return -1;
  sockets[i].type = PCB_MACRAW;
  sockets[i].born_epoch = U2_CmdEpoch();
  set_status(i, W5100_SN_SR_SOCK_MACRAW);
  /* ip65 DHCP uses MACRAW with Ethernet SA = SHAR. CYW43 transmits frames as-is; APs expect
   * Ethernet source MAC == STA hwaddr. Align SHAR (and TX SA below) with lwIP netif. */
  struct netif *sta = u2_cyw43_sta_netif();
  if (cyw43_is_initialized(&cyw43_state) && sta->hwaddr_len == 6)
    U2_SetStationMacFromBytes(sta->hwaddr);
  if (i == 0) {
    __atomic_store_n(&u2_macraw_hook, 1, __ATOMIC_RELEASE);
    U2_RequestCore0NetPoll();
  }
  return 0;
}

int U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !data || len == 0 ||
      len > U2_MACRAW_MAX_FRAME)
    return -1;
  if (get_core_num() != 0)
    return U2_Net_SendMacrawRing(i, data, 0xFFFFu, 0, len);
  U2_LWIP_BEGIN();
#if U2_ETH_HEADER_TRACE
  u2_eth_trace_tap("direct", data, len);
#endif
  bool ok = u2_send_macraw_core0(i, data, len);
  U2_LWIP_END();
  return ok ? 0 : -1;
}

void U2_Net_FeedMacrawRx(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_MACRAW || !push_rx_macraw_cb || !data)
    return;
  push_rx_macraw_cb(i, data, len);
}

static void u2_net_close_core0(int i);

/* U2_Init() closes every socket before cyw43_arch_init(). The async-context lock
 * is a null pointer until then; taking it hard-faults before USB, UART, and core 1. */
static bool u2_lwip_enter(void) {
  if (!cyw43_is_initialized(&cyw43_state))
    return false;
  U2_LWIP_BEGIN();
  return true;
}

static void u2_lwip_exit(bool held) {
  if (held)
    U2_LWIP_END();
}

static void u2_free_pending(int i) {
  if (!u2_lwip_enter()) {
    sockets[i].pend_conn = nullptr;
    sockets[i].pend_tcp = nullptr;
    sockets[i].pend_udp = nullptr;
    sockets[i].pend_raw = nullptr;
    sockets[i].silent_abort = 0;
    __atomic_store_n(&sockets[i].need_close, 0, __ATOMIC_RELEASE);
    return;
  }
  if (sockets[i].pend_conn) {
    struct tcp_pcb *c = sockets[i].pend_conn;
    sockets[i].pend_conn = nullptr;
    u2_release_tcp_arg(c);
    u2_tcp_kill(c);
  }
  if (sockets[i].pend_tcp) {
    struct tcp_pcb *t = sockets[i].pend_tcp;
    sockets[i].pend_tcp = nullptr;
    u2_release_tcp_arg(t);
    u2_tcp_kill(t);
  }
  if (sockets[i].pend_udp) {
    struct udp_pcb *u = sockets[i].pend_udp;
    sockets[i].pend_udp = nullptr;
    GetNetworkPump().DestroyUdpPcb(u);
  }
  if (sockets[i].pend_raw) {
    struct raw_pcb *r = sockets[i].pend_raw;
    sockets[i].pend_raw = nullptr;
    raw_recv(r, nullptr, nullptr);
    raw_remove(r);
  }
  sockets[i].silent_abort = 0;
  sockets[i].born_epoch = 0;
  u2_lwip_exit(true);
  __atomic_store_n(&sockets[i].need_close, 0, __ATOMIC_RELEASE);
}

void U2_BUS_RAM(U2_Net_RequestReset)(void) {
  __atomic_store_n(&u2_net_reset_pending, 1, __ATOMIC_RELEASE);
  U2_RequestCore0NetPoll();
}

/* Bus core (core 1). Never takes the lwIP lock: cyw43_arch_lwip_begin from here stalls the PIO
 * prefetch (Sn_CR comes back as the mode byte $03) and can run while core 0 is inside the pcb.
 * A live pcb is not touched; it is flagged for core 0 (need_close / silent_abort). */
void U2_BUS_RAM(U2_Net_CloseBus)(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return;
  U2_BaTrace("closeBus", i, sockets[i].type, sockets[i].status);
  if (sockets[i].type == PCB_MACRAW) {
    sockets[i].type = PCB_NONE;
    sockets[i].tx_fin_sent = 0;
    sockets[i].retire_listen = 0;
    set_status(i, W5100_SN_SR_CLOSED);
    u2_macraw_tx_queue_clear();
    u2_pnat_reset();
    if (i == 0)
      __atomic_store_n(&u2_macraw_hook, 0, __ATOMIC_RELEASE);
    if (sockets[i].hold)
      __atomic_store_n(&sockets[i].need_close, 1, __ATOMIC_RELEASE);
    U2_RequestCore0NetPoll();
    return;
  }
  if (sockets[i].type == PCB_TCP || sockets[i].type == PCB_UDP || sockets[i].type == PCB_IPRAW) {
    sockets[i].silent_abort = 1;
    set_status(i, W5100_SN_SR_CLOSED);
    __atomic_store_n(&sockets[i].need_close, 1, __ATOMIC_RELEASE);
    U2_RequestCore0NetPoll();
    return;
  }
  set_status(i, W5100_SN_SR_CLOSED);
  if (i == 0 && u2_saved_netif_input)
    __atomic_store_n(&u2_macraw_hook, 0, __ATOMIC_RELEASE);
  U2_RequestCore0NetPoll();
}

void U2_Net_Close(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return;
  if (get_core_num() != 0) {
    U2_Net_CloseBus(i);
    return;
  }
  u2_net_close_core0(i);
}

static void u2_net_close_core0(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return;
  U2_BaTrace("close0", i, sockets[i].type, sockets[i].status);
  __atomic_store_n(&sockets[i].need_close, 0, __ATOMIC_RELEASE);
  if (sockets[i].type == PCB_MACRAW && i == 0)
    __atomic_store_n(&u2_macraw_hook, 0, __ATOMIC_RELEASE);
  bool held = u2_lwip_enter();
  if (i == 0 && held &&
      __atomic_load_n(&u2_macraw_hook, __ATOMIC_ACQUIRE) == 0) {
    struct netif *sta = u2_cyw43_sta_netif();
    netif_input_fn real = u2_saved_netif_input;
    if (!real || real == u2_netif_input_wrapper)
      real = u2_sta_input_real;
    if (real && sta->input == u2_netif_input_wrapper)
      sta->input = real;
    u2_saved_netif_input = NULL;
  }
  u2_net_socket_t *s = &sockets[i];
  if (s->type == PCB_UDP && s->pcb.udp) {
    if (held) {
      U2_MARK();
      GetNetworkPump().DestroyUdpPcb(s->pcb.udp);
      U2_MARK();
    }
    s->pcb.udp = NULL;
  } else if (s->type == PCB_TCP) {
    s->silent_abort = 1;
    if (s->tcp_connected) {
      struct tcp_pcb *c = s->tcp_connected;
      s->tcp_connected = NULL;
      if (held) {
        U2_MARK();
        u2_release_tcp_arg(c);
        U2_MARK();
        u2_tcp_kill(c);
        U2_MARK();
      }
    }
    if (s->pcb.tcp) {
      struct tcp_pcb *t = s->pcb.tcp;
      s->pcb.tcp = NULL;
      if (held) {
        U2_MARK();
        u2_release_tcp_arg(t);
        U2_MARK();
        u2_tcp_kill(t);
        U2_MARK();
      }
    }
    s->silent_abort = 0;
  } else if (s->type == PCB_IPRAW && s->raw) {
    if (held) {
      U2_MARK();
      raw_recv(s->raw, nullptr, nullptr);
      raw_remove(s->raw);
      U2_MARK();
    }
    s->raw = NULL;
  } else if (s->type == PCB_MACRAW) {
    u2_macraw_tx_queue_clear();
    u2_pnat_reset();
  }
  s->tx_fin_sent = 0;
  s->retire_listen = 0;
  s->nd = 0;
  if (s->hold) {
    pbuf_free(s->hold);
    s->hold = nullptr;
  }
  s->hold_noted = 0;
  s->type = PCB_NONE;
  s->born_epoch = 0;
  set_status(i, W5100_SN_SR_CLOSED);
  u2_lwip_exit(held);
}

int U2_Net_DisconTcp(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP)
    return 0;
  if (u2_need_close(i))
    return -1;
  struct tcp_pcb *pcb = sockets[i].tcp_connected ? sockets[i].tcp_connected : sockets[i].pcb.tcp;
  if (!pcb) {
    set_status(i, W5100_SN_SR_CLOSED);
    return 0;
  }
  uint8_t st = get_status(i);
  if (st != W5100_SN_SR_ESTABLISHED && st != W5100_SN_SR_SOCK_CLOSE_WAIT &&
      st != W5100_SN_SR_SOCK_FIN_WAIT && st != W5100_SN_SR_SOCK_LAST_ACK)
    return -1;
  U2_LWIP_BEGIN();
  sockets[i].tx_fin_sent = 1;
  err_t e = tcp_shutdown(pcb, 0, 1);
  U2_LWIP_END();
  if (e != ERR_OK) {
    sockets[i].tx_fin_sent = 0;
    u2_net_close_core0(i);
    U2_SocketIrq(i, W5100_SN_IR_TIMEOUT);
    return -1;
  }
  if (st == W5100_SN_SR_SOCK_CLOSE_WAIT) {
    /* Peer already FINed. The FIN is queued. The IIGS reads Sn_SR as soon as
     * DISCON's command byte clears, and that read is CLOSED. LAST_ACK is
     * still the pcb's state until the ACK; publishing it here is what left
     * $0212 and $0213 at $1D. Drop our pointer so that ACK cannot write $00
     * onto the next test's socket. */
    set_status(i, W5100_SN_SR_CLOSED);
    if (sockets[i].tcp_connected == pcb)
      sockets[i].tcp_connected = nullptr;
    if (sockets[i].pcb.tcp == pcb)
      sockets[i].pcb.tcp = nullptr;
    /* Park it for u2_free_pending. Leaving it only on lwIP's list leaked the
     * pcb: the slot type is NONE, so reset skipped it. */
    sockets[i].pend_tcp = pcb;
    if (sockets[i].type == PCB_TCP)
      sockets[i].type = PCB_NONE;
    sockets[i].tx_fin_sent = 0;
    return 0;
  }
  if (st == W5100_SN_SR_ESTABLISHED)
    set_status(i, W5100_SN_SR_SOCK_FIN_WAIT);
  return 0;
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
  sockets[i].born_epoch = U2_CmdEpoch();
  return 0;
}

static u8_t u2_raw_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
  (void)pcb;
  int i = (int)(uintptr_t)arg - 1;
  if (!p)
    return 0;
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_IPRAW || !push_rx_cb ||
      u2_need_close(i)) {
    pbuf_free(p);
    return 1;
  }
  if (p->tot_len < 20) {
    pbuf_free(p);
    return 1;
  }
  uint8_t hdr[20];
  pbuf_copy_partial(p, hdr, 20, 0);
  uint16_t hlen = (uint16_t)((hdr[0] & 0x0Fu) * 4u);
  if (hlen < 20 || p->tot_len < hlen) {
    pbuf_free(p);
    return 1;
  }
  uint16_t plen = (uint16_t)(p->tot_len - hlen);
  std::vector<uint8_t> buf(plen ? plen : 1);
  if (plen)
    pbuf_copy_partial(p, buf.data(), plen, hlen);
  const ip4_addr_t *ip4 = ip_2_ip4(addr);
  uint32_t ip = ((uint32_t)ip4_addr1(ip4) << 24) | ((uint32_t)ip4_addr2(ip4) << 16) |
                ((uint32_t)ip4_addr3(ip4) << 8) | (uint32_t)ip4_addr4(ip4);
  push_rx_cb(i, buf.data(), plen, 2, ip, 0);
  pbuf_free(p);
  return 1;
}

int U2_Net_OpenIpraw(int i, uint8_t proto) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return -1;
  U2_Net_Close(i);
  U2_LWIP_BEGIN();
  struct raw_pcb *pcb = raw_new(proto);
  if (!pcb) {
    U2_LWIP_END();
    return -1;
  }
  ip_addr_t any;
  ip_addr_set_any(0, &any);
  raw_bind(pcb, &any);
  raw_recv(pcb, u2_raw_recv, (void *)(uintptr_t)(i + 1));
  sockets[i].raw = pcb;
  sockets[i].type = PCB_IPRAW;
  sockets[i].born_epoch = U2_CmdEpoch();
  U2_LWIP_END();
  return 0;
}

void U2_Net_SetTcpNoDelay(int i, int enable) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return;
  sockets[i].nd = enable ? 1 : 0;
  if (!enable || sockets[i].type != PCB_TCP)
    return;
  struct tcp_pcb *pcb = sockets[i].tcp_connected ? sockets[i].tcp_connected : sockets[i].pcb.tcp;
  /* Listen pcbs are a different object. ND is applied to the child in the accept callback. */
  if (pcb && get_status(i) != W5100_SN_SR_SOCK_LISTEN)
    tcp_nagle_disable(pcb);
}

static int u2_tcp_still_installed(struct tcp_pcb *pcb) {
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++) {
    if (sockets[i].pcb.tcp == pcb || sockets[i].tcp_connected == pcb ||
        sockets[i].pend_tcp == pcb || sockets[i].pend_conn == pcb)
      return 1;
  }
  return 0;
}

/* A DISCON that already published CLOSED, or a listen that lost its slot,
 * still occupies a TCP pcb. tcp_new then fails and the next OPEN stays $00. */
static void u2_reclaim_orphan_tcp(void) {
  struct tcp_pcb *lists[4];
  lists[0] = tcp_active_pcbs;
  lists[1] = tcp_tw_pcbs;
  lists[2] = tcp_bound_pcbs;
  lists[3] = tcp_listen_pcbs.pcbs;
  struct tcp_pcb *doomed[8];
  int n = 0;
  for (int L = 0; L < 4 && n < 8; L++) {
    for (struct tcp_pcb *p = lists[L]; p && n < 8; p = p->next) {
      auto *a = static_cast<U2TcpArg *>(p->callback_arg);
      if (!a || a->pcb != p)
        continue;
      if (a->sock_index < 0 || a->sock_index >= U2_NET_MAX_SOCKETS)
        continue;
      if (u2_tcp_still_installed(p))
        continue;
      doomed[n++] = p;
    }
  }
  for (int i = 0; i < n; i++) {
    struct tcp_pcb *p = doomed[i];
    if (p->state == LISTEN) {
      void *arg = p->callback_arg;
      tcp_arg(p, nullptr);
      tcp_accept(p, nullptr);
      delete static_cast<U2TcpArg *>(arg);
      u2_tcp_kill(p);
    } else {
      u2_release_tcp_arg(p);
      u2_tcp_kill(p);
    }
  }
}

int U2_Net_OpenTcp(int i) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS)
    return -1;
  U2_Net_Close(i);
  U2_LWIP_BEGIN();
  U2_MARK();
  struct tcp_pcb *pcb = tcp_new();
  if (!pcb) {
    U2_MARK();
    u2_reclaim_orphan_tcp();
    U2_MARK();
    pcb = tcp_new();
  }
  U2_MARK();
  if (!pcb) {
    U2_BaTrace("tcp_new_FAIL", i, 0, 0);
    U2_LWIP_END();
    return -1;
  }
  u2_attach_tcp_pcb(pcb, i);
  sockets[i].pcb.tcp = pcb;
  sockets[i].tcp_connected = NULL;
  sockets[i].type = PCB_TCP;
  sockets[i].born_epoch = U2_CmdEpoch();
  U2_LWIP_END();
  return 0;
}

int U2_Net_ConnectTcpEx(int i, uint32_t dest_ip_net, uint16_t dest_port, uint16_t local_port) {
  int why = 0;
  err_t err = ERR_OK;
  int link = -1;
  uint8_t ep0 = U2_CmdEpoch();
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !sockets[i].pcb.tcp)
    why = 1;
  else {
    ip_addr_t addr;
    IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF, (dest_ip_net >> 8) & 0xFF,
             dest_ip_net & 0xFF);
    U2_LWIP_BEGIN();
    link = (int)cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (local_port != 0) {
      /* $0202 and $0203 both bind local port 50023. A pcb still in TIME_WAIT
       * from the previous test would make the second bind fail and the row
       * stay CLOSED. */
      ip_set_option(sockets[i].pcb.tcp, SOF_REUSEADDR);
      err_t br = tcp_bind(sockets[i].pcb.tcp, IP4_ADDR_ANY, local_port);
      if (br != ERR_OK) {
        err = br;
        why = 2;
      }
    }
    if (why == 0) {
      err = tcp_connect(sockets[i].pcb.tcp, &addr, dest_port, u2_tcp_connected_cb);
      /* tcp_connect() assigns rcv_wnd = TCP_WND and has already built the SYN.
       * Pull the pcb back to the socket buffer for every later ACK. */
      if (err == ERR_OK) {
        int sz = 0, live = 0, sh = 0;
        U2_RxDebugStat(i, &sz, &live, &sh);
        /* An 8K socket is already TCP_WND. Shrinking it by one byte leaves
         * rcv_ann_right_edge one past rcv_wnd. Leave the window tcp_connect
         * installed. A 2K socket still has to advertise the socket, not 8K. */
        if (sz > 1 && sz < TCP_WND) {
          tcpwnd_size_t w = (tcpwnd_size_t)(sz - 1);
          sockets[i].pcb.tcp->rcv_wnd = w;
          sockets[i].pcb.tcp->rcv_ann_wnd = w;
          /* Do not write rcv_ann_right_edge. Moving that edge backward makes
           * lwIP advertise window 0 for the rest of the connection. */
          // #region agent log
          U2_AgentLog("H15", "wnd", (int)w, (int)TCP_WND, sz, 1);
          // #endregion
        }
      }
      if (err == ERR_OK) {
        /* MR $80 during tcp_connect belongs to the next test. Publishing
         * SYNSENT here would be that test's Sn_SR. */
        if (U2_CmdEpoch() != ep0)
          why = 4;
        else
          set_status(i, W5100_SN_SR_SOCK_SYNSENT);
      } else
        why = 3;
    }
    U2_LWIP_END();
  }
  // #region agent log
  /* why: 1=no pcb, 2=bind, 3=tcp_connect. b=lwIP err. c=STA link (0 down, 3 up). */
  U2_AgentLog("H8", "syn", why, (int)err, link, 1);
  // #endregion
  return why ? -1 : 0;
}

int U2_Net_ListenTcp(int i, uint16_t local_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !sockets[i].pcb.tcp)
    return -1;
  uint8_t ep0 = U2_CmdEpoch();
  U2_LWIP_BEGIN();
  struct tcp_pcb *pcb = sockets[i].pcb.tcp;
  /* tcp_listen requires CLOSED. A pcb left in another state makes lwIP return
   * ERR_VAL and the defer path then publishes CLOSED, but a LISTEN that never
   * enters this function stays at SOCK_INIT. If this one is already listening,
   * publish that. Otherwise replace it with a fresh CLOSED pcb. */
  if (pcb->state == LISTEN) {
    if (U2_CmdEpoch() == ep0)
      set_status(i, W5100_SN_SR_SOCK_LISTEN);
    U2_LWIP_END();
    return U2_CmdEpoch() == ep0 ? 0 : -1;
  }
  if (pcb->state != CLOSED) {
    u2_release_tcp_arg(pcb);
    u2_tcp_kill(pcb);
    pcb = tcp_new();
    if (!pcb) {
      u2_reclaim_orphan_tcp();
      pcb = tcp_new();
    }
    if (!pcb) {
      sockets[i].pcb.tcp = nullptr;
      sockets[i].type = PCB_NONE;
      U2_LWIP_END();
      return -1;
    }
    u2_attach_tcp_pcb(pcb, i);
    sockets[i].pcb.tcp = pcb;
    sockets[i].tcp_connected = nullptr;
  }
  ip_set_option(pcb, SOF_REUSEADDR);
  err_t err = tcp_bind(pcb, IP4_ADDR_ANY, local_port);
  if (err != ERR_OK) {
    U2_LWIP_END();
    return -1;
  }
  u2_release_tcp_arg(pcb);
  struct tcp_pcb *listen = tcp_listen_with_backlog(pcb, 1);
  if (!listen) {
    U2_LWIP_END();
    return -1;
  }
  sockets[i].pcb.tcp = listen;
  u2_attach_tcp_pcb(listen, i);
  tcp_accept(listen, u2_tcp_accept_cb);
  if (U2_CmdEpoch() != ep0) {
    U2_LWIP_END();
    return -1;
  }
  set_status(i, W5100_SN_SR_SOCK_LISTEN);
  U2_BaTrace("listen_ok", i, (int32_t)local_port, (int32_t)ip4_addr_get_u32(netif_ip4_addr(u2_cyw43_sta_netif())));
  U2_LWIP_END();
  return 0;
}

int U2_Net_SendUdp(int i, const uint8_t *ring_base, uint16_t ring_size, uint16_t start_off,
                   uint16_t len, uint32_t dest_ip_net, uint16_t dest_port) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_UDP || !sockets[i].pcb.udp ||
      !ring_base || ring_size == 0 || len == 0)
    return -1;
  U2_LWIP_BEGIN();
  /* PBUF_RAM gives a single contiguous payload; copy from the (possibly wrapping) TX ring. */
  struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
  err_t e = ERR_MEM;
  if (p) {
    uint16_t mask = (uint16_t)(ring_size - 1);
    uint8_t *dst = (uint8_t *)p->payload;
    for (uint16_t j = 0; j < len; j++)
      dst[j] = ring_base[(uint16_t)((start_off + j) & mask)];
    ip_addr_t addr;
    IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF, (dest_ip_net >> 8) & 0xFF,
             dest_ip_net & 0xFF);
    e = udp_sendto(sockets[i].pcb.udp, p, &addr, dest_port);
    pbuf_free(p);
  }
  U2_BaTrace("udp_tx", i, (int32_t)len, (int32_t)e);
  U2_LWIP_END();
  return (e == ERR_OK) ? 0 : -1;
}

int U2_Net_SendIpraw(int i, const uint8_t *ring_base, uint16_t ring_size, uint16_t start_off,
                     uint16_t len, uint32_t dest_ip_net) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_IPRAW || !sockets[i].raw ||
      !ring_base || ring_size == 0 || len == 0)
    return -1;
  U2_LWIP_BEGIN();
  struct pbuf *p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
  err_t e = ERR_MEM;
  if (p) {
    uint16_t mask = (uint16_t)(ring_size - 1);
    uint8_t *dst = (uint8_t *)p->payload;
    for (uint16_t j = 0; j < len; j++)
      dst[j] = ring_base[(uint16_t)((start_off + j) & mask)];
    ip_addr_t addr;
    IP4_ADDR(&addr, (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF, (dest_ip_net >> 8) & 0xFF,
             dest_ip_net & 0xFF);
    e = raw_sendto(sockets[i].raw, p, &addr);
    pbuf_free(p);
  }
  U2_LWIP_END();
  return (e == ERR_OK) ? 0 : -1;
}

int U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || sockets[i].type != PCB_TCP || !data)
    return -1;
  if (len == 0)
    return 0;
  struct tcp_pcb *pcb = sockets[i].tcp_connected ? sockets[i].tcp_connected : sockets[i].pcb.tcp;
  if (!pcb)
    return -1;
  U2_LWIP_BEGIN();
  uint16_t avail = tcp_sndbuf(pcb);
  uint16_t to_write = (len < avail) ? len : avail;
  int accepted = 0;
  if (to_write > 0) {
    err_t err = tcp_write(pcb, data, to_write, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
      accepted = (int)to_write;
      tcp_output(pcb);
    } else if (err != ERR_MEM) {
      accepted = -1; /* fatal (e.g. not connected) */
    }
    /* ERR_MEM => accepted stays 0; remainder is left in the W5100 TX ring. */
  }
  U2_LWIP_END();
  return accepted;
}

static volatile uint32_t u2_tcp_rx_credit[U2_NET_MAX_SOCKETS];

void U2_BUS_RAM(U2_Net_NoteRecv)(int i, uint16_t n) {
  if (i < 0 || i >= U2_NET_MAX_SOCKETS || n == 0)
    return;
  __atomic_fetch_add(&u2_tcp_rx_credit[i], (uint32_t)n, __ATOMIC_RELAXED);
}

void U2_BUS_RAM(U2_Net_RecvConfirm)(int i) { (void)i; }

uint8_t U2_BUS_RAM(U2_Net_GetStatus)(int i) { return get_status(i); }

void U2_BUS_RAM(U2_Net_SetStatus)(int i, uint8_t status) { set_status(i, status); }

static void u2_macraw_hook_apply(void) {
  uint8_t want = __atomic_load_n(&u2_macraw_hook, __ATOMIC_ACQUIRE);
  if (!want && !u2_saved_netif_input && !u2_sta_input_real)
    return;
  if (!u2_lwip_enter())
    return;
  struct netif *sta = u2_cyw43_sta_netif();
  want = __atomic_load_n(&u2_macraw_hook, __ATOMIC_ACQUIRE);
  if (u2_saved_netif_input == u2_netif_input_wrapper)
    u2_saved_netif_input = NULL;
  if (sta->input && sta->input != u2_netif_input_wrapper && !u2_sta_input_real)
    u2_sta_input_real = sta->input;
  if (want) {
    if (sta->input && sta->input != u2_netif_input_wrapper) {
      u2_saved_netif_input = sta->input;
      if (!u2_sta_input_real)
        u2_sta_input_real = sta->input;
      sta->input = u2_netif_input_wrapper;
    }
  } else if (sockets[0].type != PCB_MACRAW) {
    netif_input_fn real = u2_saved_netif_input ? u2_saved_netif_input : u2_sta_input_real;
    if (real && sta->input == u2_netif_input_wrapper)
      sta->input = real;
    u2_saved_netif_input = NULL;
  }
  u2_lwip_exit(true);
}

/* Core 0 view of "is the Apple using the slot": any $C0C4-$C0C7 cycle since the last look. */
static uint32_t u2_act_n;
static uint32_t u2_act_ms;
static bool u2_act_seen;
static void u2_note_bus_activity(void) {
  uint32_t n = U2_BusCycleCount();
  if (n != u2_act_n) {
    u2_act_n = n;
    u2_act_ms = to_ms_since_boot(get_absolute_time());
    u2_act_seen = true;
  }
}

int U2_BusActiveRecently(uint32_t window_ms) {
  u2_note_bus_activity();
  return u2_act_seen && (uint32_t)(to_ms_since_boot(get_absolute_time()) - u2_act_ms) < window_ms;
}

void U2_Net_ServicePoll(void) {
  /* cyw43/lwIP poll is core-0 only; U2_Poll() may run on core 1 from bus loop. */
  if (get_core_num() != 0)
    return;
  U2_BaCore0Poll();
  u2_note_bus_activity();
  if (__atomic_exchange_n(&u2_net_reset_pending, 0, __ATOMIC_ACQ_REL)) {
    /* MR $80 already bumped u2_cmd_epoch and cleared the register file on the bus core, in
     * one step (u2_reset, section 1ev). All that is left for core 0 is to drop the pcbs.
     * Pcbs opened after the bump (born_epoch == now) belong to the test that
     * has already continued. Closing those is what stored $00. */
    uint8_t ep = U2_CmdEpoch();
    U2_BaNoteResetServed(1);
    U2_BaStage((4u << 28) | 2u);
    if (!(sockets[0].type == PCB_MACRAW && sockets[0].born_epoch == ep))
      __atomic_store_n(&u2_macraw_hook, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < U2_NET_MAX_SOCKETS; i++) {
      if (sockets[i].born_epoch == ep)
        continue;
      /* A queued command does not keep the previous pcb. Leaving it allocated
       * filled the TCP pool, and later OPEN stayed at CLOSED ($00). The new
       * command allocates its own pcb. */
      if (sockets[i].type == PCB_NONE && !sockets[i].need_close && !sockets[i].pend_tcp &&
          !sockets[i].pend_conn && !sockets[i].pend_udp && !sockets[i].pend_raw)
        continue;
      if (sockets[i].born_epoch == ep)
        continue;
      u2_net_close_core0(i);
    }
  }
  U2_BaStage((4u << 28) | 3u);
  u2_macraw_hook_apply();
  U2_BaStage((4u << 28) | 4u);
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++) {
    /* Live pcbs stay on the socket until this core can take the lock. A core-1 detach
     * during ListenTcp/OpenTcp freed the pcb out from under lwIP and wedged later OPENs. */
    if (u2_need_close(i))
      u2_net_close_core0(i);
    if (sockets[i].pend_tcp || sockets[i].pend_conn || sockets[i].pend_udp || sockets[i].pend_raw)
      u2_free_pending(i);
    if (sockets[i].retire_listen && sockets[i].type == PCB_TCP && sockets[i].pcb.tcp) {
      struct tcp_pcb *listen = sockets[i].pcb.tcp;
      sockets[i].pcb.tcp = nullptr;
      sockets[i].retire_listen = 0;
      U2_LWIP_BEGIN();
      u2_release_tcp_arg(listen);
      tcp_close(listen);
      U2_LWIP_END();
    }
  }
  U2_BaStage((4u << 28) | 5u);
  U2_ProcessDeferredSocketCmds();
  U2_BaStage((4u << 28) | 6u);
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++) {
    uint32_t credit = __atomic_exchange_n(&u2_tcp_rx_credit[i], 0, __ATOMIC_ACQ_REL);
    if (credit && sockets[i].type == PCB_TCP) {
      struct tcp_pcb *tpcb = u2_tcp_pcb(i);
      if (tpcb) {
        U2_LWIP_BEGIN();
        while (credit) {
          u16_t chunk = credit > 65535u ? 65535u : (u16_t)credit;
          tcp_recved(tpcb, chunk);
          credit -= chunk;
        }
        /* 40-byte RECVs do not move lwIP's right edge (threshold is one MSS),
         * so rcv_ann_wnd sticks at 0 after the visualization preload. Step the
         * edge forward to the bytes the socket can still take. */
        if (tpcb->rcv_ann_wnd == 0 && tpcb->rcv_wnd != 0) {
          tpcb->rcv_ann_wnd = tpcb->rcv_wnd;
          tpcb->rcv_ann_right_edge = tpcb->rcv_nxt + tpcb->rcv_wnd;
          tcp_output(tpcb);
        }
        U2_LWIP_END();
      }
    }
  }
  /* echo_once closes after the payload, which FINs. If that FIN was parked
   * on refused_data, tcp_fasttmr would not deliver it for 250 ms, and
   * finish_io would still read ESTABLISHED. Deliver it on this poll, then
   * publish CLOSE_WAIT if the callback did not. */
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++) {
    if (sockets[i].type != PCB_TCP)
      continue;
    struct tcp_pcb *tpcb = u2_tcp_pcb(i);
    if (!tpcb)
      continue;
    if (tpcb->state != ESTABLISHED && tpcb->state != CLOSE_WAIT)
      continue;
    U2_LWIP_BEGIN();
    if (tpcb->refused_data)
      tcp_process_refused_data(tpcb);
    struct tcp_pcb *live = u2_tcp_pcb(i);
    if (live && live->state == CLOSE_WAIT &&
        get_status(i) == W5100_SN_SR_ESTABLISHED) {
      set_status(i, W5100_SN_SR_SOCK_CLOSE_WAIT);
      U2_SocketIrq(i, W5100_SN_IR_DISCON);
    }
    U2_LWIP_END();
  }
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++) {
    if (sockets[i].hold && sockets[i].type == PCB_TCP) {
      U2_LWIP_BEGIN();
      u2_flush_hold(i);
      U2_LWIP_END();
      // #region agent log
      if (sockets[i].hold) {
        static uint32_t u2_probe_us;
        uint32_t now = time_us_32();
        if ((uint32_t)(now - u2_probe_us) > 500000u) {
          u2_probe_us = now;
          int cr = 0, bus = 0, rd = 0;
          U2_RxDebugProbe(i, &cr, &bus, &rd);
          U2_AgentLog("H12", "rx-probe", cr, bus, rd, 1);
          int irq = pio_interrupt_get(pio0, 0) ? 1 : 0;
#ifndef PICO_RP2040
          int pc = (int)pio_sm_get_pc(pio0, SM_A2BUS);
#else
          int pc = (int)pio_sm_get_pc(pio0, 0);
#endif
          int lf = (int)pio_sm_get_rx_fifo_level(pio0, SM_LISTENER);
          U2_AgentLog("H14", "rx-bus", irq, pc, lf, 1);
        }
      }
      // #endregion
    }
  }
  // #region agent log
  /* H16: window closed so the peer stopped (no more rx callbacks).
   * H17: 6502 died mid-read — bus_n stops while the Pico is still polling.
   * Printed even when no segment arrives, which the sampled rx lines hide. */
  {
    static uint32_t u2_hb_us;
    uint32_t now = time_us_32();
    if ((uint32_t)(now - u2_hb_us) > 500000u) {
      u2_hb_us = now;
      for (int i = 0; i < U2_NET_MAX_SOCKETS; i++) {
        if (sockets[i].type != PCB_TCP || !u2_tcp_pcb(i))
          continue;
        int wnd = 0, ann = 0;
        U2_LWIP_BEGIN();
        struct tcp_pcb *tpcb = u2_tcp_pcb(i);
        if (tpcb) {
          wnd = (int)tpcb->rcv_wnd;
          ann = (int)tpcb->rcv_ann_wnd;
        }
        U2_LWIP_END();
        int sz = 0, live = 0, sh = 0;
        U2_RxDebugStat(i, &sz, &live, &sh);
        int cr = 0, bus = 0, rd = 0;
        U2_RxDebugProbe(i, &cr, &bus, &rd);
        int hold = sockets[i].hold ? (int)sockets[i].hold->tot_len : 0;
        U2_AgentLog("H16", "hb", wnd, live, rd, 1);
        U2_AgentLog("H17", "hb2", bus, ann, hold, 1);
        U2_AgentLog("H19", "hb3", (int)u2_dbg_push_n, rd, live, 1);
        {
          int qw = 0, qf = 0, ql = 0;
          U2_RxDebugQueue(&qw, &qf, &ql);
          U2_AgentLog("H24", "q", qw, qf, ql, 1);
        }
        // #region agent log
        /* H23: once the read pointer has reached the audio and the bus then
         * repeats, record whether the data-port state machine is stuck. */
        {
          static int u2_prev_bus = -1;
          static uint8_t u2_pio_logged;
          if (!u2_pio_logged && rd >= 20000 && bus == u2_prev_bus) {
            u2_pio_logged = 1;
            int irq = pio_interrupt_get(pio0, 0) ? 1 : 0;
#ifndef PICO_RP2040
            int pc = (int)pio_sm_get_pc(pio0, SM_A2BUS);
#else
            int pc = (int)pio_sm_get_pc(pio0, 0);
#endif
            uint32_t stall = pio0->fdebug & (1u << SM_LISTENER);
            if (stall)
              pio0->fdebug = stall;
            U2_AgentLog("H23", "pio", irq, pc, stall ? 1 : 0, 1);
            {
              /* H30: a = D0-D7 output-enable (0 = released). b = nPICOWR level
               * (active low; 1 = not strobing). c = listener PC. */
              uint32_t oe = pio0->dbg_padoe;
              int lpc = (int)pio_sm_get_pc(pio0, SM_LISTENER);
              U2_AgentLog("H30", "oe", (int)((oe >> 11) & 0xFFu), gpio_get(22), lpc, 1);
            }
            {
              int aim = -1, at = -1, seen = 0;
              U2_RxDebugAim(&aim, &at, &seen);
              U2_AgentLog("H28", "aim", aim, at, seen, 1);
              for (int slot = 0; slot < 8; slot++) {
                int adr = -1, packed = -1, pk = -1;
                U2_RxDebugCyc(slot, &adr, &packed, &pk);
                U2_AgentLog("H29", "cy", adr, packed, pk, 1);
              }
            }
          }
          u2_prev_bus = bus;
        }
        // #endregion
        break;
      }
    }
  }
  // #endregion
  if (cyw43_is_initialized(&cyw43_state)) {
    struct netif *sta = u2_cyw43_sta_netif();
    const ip4_addr_t *ip = netif_ip4_addr(sta);
    if (netif_is_up(sta) && !ip4_addr_isany(ip)) {
      const ip4_addr_t *gw = netif_ip4_gw(sta);
      const ip4_addr_t *nm = netif_ip4_netmask(sta);
      uint8_t ipb[4] = {ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip)};
      uint8_t gwb[4] = {ip4_addr1(gw), ip4_addr2(gw), ip4_addr3(gw), ip4_addr4(gw)};
      uint8_t nmb[4] = {ip4_addr1(nm), ip4_addr2(nm), ip4_addr3(nm), ip4_addr4(nm)};
      U2_MirrorStaNet(ipb, gwb, nmb);
    }
  }
#if U2_ETH_HEADER_TRACE
  u2_eth_trace_try_install();
#endif
  u2_macraw_tx_drain();
#if U2_RX_AUDIT
  /* #region agent log
   * Deliberately outside the NDEBUG guard: the audit has to run in a Release build so the bus
   * path keeps its normal timing (the Debug monitor's blocking UART changes what we measure).
   * H4: time between consecutive core-0 services. lwIP/CYW43 live here, so a long gap is a
   * direct measure of the starvation that would produce Contiki's stall-then-update pattern. */
  {
    static uint64_t u2_last_poll_us;
    uint64_t now_us = time_us_64();
    if (u2_last_poll_us) {
      uint32_t gap = (uint32_t)(now_us - u2_last_poll_us);
      if (gap > g_u2_core0_gap_max_us) g_u2_core0_gap_max_us = gap;
      if (gap > 5000u) g_u2_core0_gap_5ms++;
      if (gap > 20000u) g_u2_core0_gap_20ms++;
      if (gap > 100000u) g_u2_core0_gap_100ms++;
    }
    g_u2_core0_polls++;

    static absolute_time_t u2_audit_next;
    if (time_reached(u2_audit_next)) {
      u2_audit_next = make_timeout_time_ms(10000);
      U2_RxAuditReport();
    }
    /* Re-sample AFTER the report: its own blocking UART write is instrumentation cost, and
     * counting it as a service gap would fake the very starvation H4 is testing for. */
    u2_last_poll_us = time_us_64();
  }
  /* #endregion */
#endif
#ifndef NDEBUG
  if (time_reached(u2_macraw_tx_stats_next)) {
    u2_macraw_tx_stats_next = make_timeout_time_ms(10000);
    uint8_t q_count;
    uint32_t sv = u2_tx_lock();
    q_count = u2_macraw_tx_q_count;
    u2_tx_unlock(sv);
    printf("[u2macraw] tx_q=%u tx_q_drop=%lu lo_err=%lu pbuf_fail=%lu\n",
           (unsigned)q_count, (unsigned long)u2_macraw_tx_q_drop,
           (unsigned long)u2_macraw_tx_lo_err, (unsigned long)u2_macraw_tx_pbuf_fail);
  }
#endif
}

void U2_Net_WaitForWork(absolute_time_t until) {
  U2_Net_ServicePoll();
  absolute_time_t slice = make_timeout_time_ms(2);
  if (absolute_time_diff_us(slice, until) < 0)
    slice = until;
  cyw43_arch_wait_for_work_until(slice);
}

void U2_Net_Poll(void) {
  if (get_core_num() != 0)
    return;
  U2_StagePoll();
  u2_core0_net_wake_pending = false; /* servicing core 1's request (§1cx) */
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
void U2_Net_RequestReset(void) {}
int U2_Net_DisconTcp(int i) {
  (void)i;
  return -1;
}
int U2_Net_OpenUdp(int i, uint16_t local_port) {
  (void)i;
  (void)local_port;
  return -1;
}
int U2_Net_OpenTcp(int i) {
  (void)i;
  return -1;
}
int U2_Net_OpenIpraw(int i, uint8_t proto) {
  (void)i;
  (void)proto;
  return -1;
}
int U2_Net_OpenMacraw(int i) {
  (void)i;
  return -1;
}
int U2_Net_SendMacrawRing(int i, const uint8_t *ring, uint16_t mask, uint16_t start, uint16_t len) {
  (void)i;
  (void)ring;
  (void)mask;
  (void)start;
  (void)len;
  return -1;
}
void U2_Net_CloseBus(int i) { (void)i; }
void U2_Net_SetTcpNoDelay(int i, int enable) {
  (void)i;
  (void)enable;
}
int U2_Net_SendMacraw(int i, const uint8_t *data, uint16_t len) {
  (void)i;
  (void)data;
  (void)len;
  return -1;
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
int U2_Net_SendUdp(int i, const uint8_t *ring_base, uint16_t ring_size, uint16_t start_off,
                   uint16_t len, uint32_t dest_ip_net, uint16_t dest_port) {
  (void)i;
  (void)ring_base;
  (void)ring_size;
  (void)start_off;
  (void)len;
  (void)dest_ip_net;
  (void)dest_port;
  return -1;
}
int U2_Net_SendIpraw(int i, const uint8_t *ring_base, uint16_t ring_size, uint16_t start_off,
                     uint16_t len, uint32_t dest_ip_net) {
  (void)i;
  (void)ring_base;
  (void)ring_size;
  (void)start_off;
  (void)len;
  (void)dest_ip_net;
  return -1;
}
int U2_Net_SendTcp(int i, const uint8_t *data, uint16_t len) {
  (void)i;
  (void)data;
  return (int)len;
}
void U2_Net_NoteRecv(int i, uint16_t n) {
  (void)i;
  (void)n;
}
void U2_Net_RecvConfirm(int i) { (void)i; }
uint8_t U2_Net_GetStatus(int i) {
  (void)i;
  return W5100_SN_SR_CLOSED;
}
void U2_Net_SetStatus(int i, uint8_t status) {
  (void)i;
  (void)status;
}
void U2_Net_ServicePoll(void) {}
void U2_Net_Poll(void) {}
void U2_Net_WaitForWork(absolute_time_t until) { (void)until; }
int U2_BusActiveRecently(uint32_t window_ms) {
  (void)window_ms;
  return 0;
}

} // extern "C"

#endif /* PICO_CYW43_ARCH_POLL */

/* Core 0. 1 when the Pico's Wi-Fi link is up and DHCP has given it an address. A real Uthernet II
 * has its PHY link and an IP from power-on; MegaFlash needs ~6-20 s after boot. Until then every
 * networked command fails with ERR_RTE. u2 deferred CONNECT/SEND wait on this (section 1fa). */
extern "C" int U2_Net_LinkReady(void) {
  if (!cyw43_is_initialized(&cyw43_state))
    return 0;
  if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
    return 0;
  return !ip4_addr_isany_val(*netif_ip4_addr(u2_cyw43_sta_netif()));
}

/* Audit only: raw link state (CYW43_LINK_*) for the transition trace. -99 before init. */
extern "C" int U2_Net_LinkStatusRaw(void) {
  if (!cyw43_is_initialized(&cyw43_state))
    return -99;
  return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
}

#if U2_BUS_AUDIT
/* Audit report helpers: the Pico's STA address as lwIP sees it. Compare with SIPR (what the 6502
 * programmed) and with the --local-ip the NetTester host connects to. */
extern "C" uint32_t U2_BaStaIp(void) {
  if (!cyw43_is_initialized(&cyw43_state))
    return 0;
  return ip4_addr_get_u32(netif_ip4_addr(u2_cyw43_sta_netif()));
}
#endif

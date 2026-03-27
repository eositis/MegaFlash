#ifndef _NETWORK_PUMP_H
#define _NETWORK_PUMP_H

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include <ctime>

extern "C" {
#include "lwip/ip_addr.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
}

#include <unordered_map>
#include <vector>

#include "network.h" /* TestResult_t, NetworkError_t */

// Forward declaration
class NetworkPump;
struct pbuf;

// lwIP UDP recv callback; arg = NetworkPump* (§14.10).
extern "C" void NetworkPump_LegacyUdpRecv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                          const ip_addr_t *addr, u16_t port);

// lwIP DNS callback; arg = INetworkSession* (issuer); pump holds `dns_pending_owner_` for stale-drop (§14.11).
extern "C" void NetworkPump_LegacyDnsCallback(const char *hostname, const ip_addr_t *ipaddr, void *arg);

// lwIP TCP: arg = INetworkSession* (pcb owner). `tcp_err` does not pass pcb; multi-pcb sessions track pcbs internally (§14.12).
extern "C" err_t NetworkPump_LegacyTcpRecv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
extern "C" void NetworkPump_LegacyTcpErr(void *arg, err_t err);

//
// INetworkSession
//
// Protocol-specific sessions (NTP, TFTP, Uthernet II, …) override the hooks they need.
// Defaults are no-ops so incremental migration is possible.
//
class INetworkSession {
public:
  virtual ~INetworkSession() {}

  /// lwIP UDP path: raw pbuf (default no-op). Used by `NetworkPump` dispatch for registered pcbs (§14.10).
  /// `pcb` identifies which UDP socket when one session owns multiple `udp_pcb`s (e.g. Uthernet II).
  virtual void OnUdpRecvPbuf(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, uint16_t port) {
    (void)pcb;
    (void)p;
    (void)addr;
    (void)port;
  }

  /// Result of async `dns_gethostbyname` when registered as pending on the pump. `ipaddr == nullptr` means invalid host.
  virtual void OnDnsGetHostByNameResult(const ip_addr_t *ipaddr) { (void)ipaddr; }

  /// lwIP TCP recv (`p == nullptr` often means peer closed). Default frees `p` if non-NULL.
  virtual void OnTcpRecvPbuf(struct tcp_pcb *pcb, struct pbuf *p, err_t err);

  /// lwIP TCP fatal/error path; lwIP does not pass `pcb` here — use for single-pcb sessions or track pcbs internally.
  virtual void OnTcpErr(err_t err) { (void)err; }

  virtual void OnStart(NetworkPump &pump) { (void)pump; }

  virtual void OnDNSResult(int dns_err, const ip_addr_t *ipaddr) {
    (void)dns_err;
    (void)ipaddr;
  }

  virtual void OnUDPReceived(const uint8_t *payload,
                               uint16_t payloadlen,
                               const ip_addr_t &remote_addr,
                               uint16_t remote_port) {
    (void)payload;
    (void)payloadlen;
    (void)remote_addr;
    (void)remote_port;
  }

  virtual void OnTCPEvent(struct tcp_pcb *pcb, uint8_t event_flags) {
    (void)pcb;
    (void)event_flags;
  }

  virtual void OnTimer(uint32_t arg) { (void)arg; }

  virtual void OnWatchdog() {}

  virtual void Abort() {}

  virtual bool IsDone() const { return true; }

  /// Cooperative work each time the shared pump runs (Core 0 idle / future async paths).
  virtual void OnPump(NetworkPump &pump) { (void)pump; }
};

//
// NetworkPump
//
// Owns shared cyw43/lwIP polling (PollOnce), session registry, and legacy RunNTP /
// RunTFTP / RunTestWifi entry points. Those paths register a short-lived
// INetworkSession adapter that drives PumpNetworkIteration from PollOnce (same
// work per iteration as CUDPTask::Run's inner loop). CUDPTask::Run remains for
// any direct callers and uses the same EnterRunSession + BeginRun + loop.
//
class NetworkPump {
public:
  enum LegacyOperationKind {
    LEGACY_OPERATION_NONE = 0,
    LEGACY_OPERATION_NTP,
    LEGACY_OPERATION_TESTWIFI,
    LEGACY_OPERATION_TFTP
  };

  NetworkPump();
  ~NetworkPump();

  // Initialise WiFi/driver if needed. Safe to call multiple times.
  void Init();

  // Ensure WiFi is connected using the given SSID/WPA key.
  // Returns true on success, false on failure.
  bool EnsureWifiConnected(const char *ssid, const char *wpakey);

  // Register/unregister sessions. The pump does not own the lifetime
  // of the session objects; callers must keep them alive.
  void AddSession(INetworkSession *session);
  void RemoveSession(INetworkSession *session);

  // Poll once: pump cyw43/lwIP, dispatch any queued events,
  // handle timers and watchdogs. Intended to be called from
  // the Core 0 loop when we move protocols over.
  void PollOnce();

  /// Register pcb owner for pump-dispatched recv (before `udp_recv`); owner usually implements `OnUdpRecvPbuf`.
  void RegisterUdpPcbOwner(struct udp_pcb *pcb, INetworkSession *owner);
  void UnregisterUdpPcb(struct udp_pcb *pcb);

  /// Register owner for `CreateTcpPcb` path (`tcp_arg` = owner). Unregistered in `DestroyTcpPcb`.
  void RegisterTcpPcbOwner(struct tcp_pcb *pcb, INetworkSession *owner);
  void UnregisterTcpPcb(struct tcp_pcb *pcb);

  /// Used only by `NetworkPump_LegacyUdpRecv` (lwIP callback); not for app code.
  void dispatchLegacyUdpRecv(struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);

  /// In-flight `dns_gethostbyname` (single slot per pump; superseded by a new lookup). Issuer must implement `OnDnsGetHostByNameResult`.
  void RegisterDnsPendingOwner(INetworkSession *owner);
  void ClearDnsPendingOwner(INetworkSession *owner);
  bool IsDnsPendingOwner(const INetworkSession *owner) const { return dns_pending_owner_ == owner; }

  // Abort all sessions (e.g. on Apple II reset). Sessions are
  // expected to clean up promptly when Abort() is called.
  void RequestAbortAll();

  // Run the existing legacy TFTP path under manager bookkeeping.
  // Returns the raw exception/error code from the underlying task.
  int32_t RunTFTP(const uint32_t taskid,
                  const uint32_t dir,
                  const uint32_t unitNum,
                  const char *hostname,
                  const char *filename,
                  const bool enable1kBlockSize,
                  const uint32_t tftpTimeout,
                  const uint32_t tftpMaxAttempt,
                  const uint16_t tftpServerPort,
                  const char *ssid,
                  const char *wpakey);

  // NTP: legacy CNTPTask path under pump bookkeeping. On success, writes Unix epoch.
  NetworkError_t RunNTP(const char *ssid, const char *wpakey, time_t *out_seconds);

  // Test WiFi / connectivity: legacy CTestWifiTask path under pump bookkeeping.
  void RunTestWifi(TestResult_t *testResultPtr, const char *ssid, const char *wpakey);

  // --- Helpers intended for future use by sessions ---

  /// Allocate/bind pcb, `RegisterUdpPcbOwner`, `udp_recv(NetworkPump_LegacyUdpRecv, this pump)`. `local_port` 0 = ephemeral.
  udp_pcb *CreateUdpPcb(INetworkSession *owner, uint16_t local_port);
  /// `UnregisterUdpPcb` + `udp_remove`.
  void DestroyUdpPcb(udp_pcb *pcb);

  /// `tcp_new`, register owner, `tcp_arg(owner)`, `tcp_recv`/`tcp_err` → `OnTcpRecvPbuf`/`OnTcpErr`.
  tcp_pcb *CreateTcpPcb(INetworkSession *owner);
  /// Unregister, detach callbacks, `tcp_abort`.
  void DestroyTcpPcb(tcp_pcb *pcb);

  /// One-shot timer; `OnTimer(arg)` on expiry. Replaces any prior timer for the same `owner`.
  void ScheduleTimer(INetworkSession *owner, uint32_t timeout_ms, uint32_t arg);
  void CancelTimer(INetworkSession *owner);

private:
  void DrainSessionTimers();
  void BeginLegacyOperation(LegacyOperationKind kind, uint32_t taskid, const char *label);
  void EndLegacyOperation();

  bool cyw43Inited;
  bool wifiConnected;
  LegacyOperationKind activeLegacyOperation;
  uint32_t activeLegacyTaskId;
  const char *activeLegacyLabel;

  static const size_t MAX_SESSIONS = 8;
  std::vector<INetworkSession *> sessions_;

  std::unordered_map<struct udp_pcb *, INetworkSession *> udp_pcb_owners_;
  std::unordered_map<struct tcp_pcb *, INetworkSession *> tcp_pcb_owners_;

  struct SessionTimer {
    INetworkSession *session;
    absolute_time_t deadline;
    uint32_t arg;
  };
  std::vector<SessionTimer> session_timers_;

  INetworkSession *dns_pending_owner_;
};

#endif // _NETWORK_PUMP_H


#ifndef _NETWORK_PUMP_H
#define _NETWORK_PUMP_H

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

extern "C" {
#include "lwip/ip_addr.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
}

// Forward declaration
class NetworkPump;

//
// INetworkSession
//
// Abstract interface implemented by protocol-specific sessions
// (e.g. NTP, TFTP RX/TX, Uthernet II TCP/UDP).
//
class INetworkSession {
public:
  virtual ~INetworkSession() {}

  // Called once when the session is added to the pump.
  virtual void OnStart(NetworkPump &pump) = 0;

  // DNS result for a hostname previously requested by this session.
  virtual void OnDNSResult(int dns_err, const ip_addr_t *ipaddr) = 0;

  // UDP payload received on a pcb owned by this session.
  virtual void OnUDPReceived(const uint8_t *payload,
                             uint16_t payloadlen,
                             const ip_addr_t &remote_addr,
                             uint16_t remote_port) = 0;

  // TCP event for a pcb owned by this session (to be refined later).
  virtual void OnTCPEvent(struct tcp_pcb *pcb, uint8_t event_flags) = 0;

  // Timer expiry for a timer this session scheduled.
  virtual void OnTimer(uint32_t arg) = 0;

  // Watchdog expiry if the session has not made progress for a while.
  virtual void OnWatchdog() = 0;

  // Request to abort the session (e.g. Apple II reset, user cancel).
  virtual void Abort() = 0;

  // Whether the session is finished (success or failure).
  virtual bool IsDone() const = 0;
};

//
// NetworkPump
//
// Single owner of cyw43/lwIP event pumping and all pcbs used
// by INetworkSession instances. This is a planning/skeleton
// implementation; existing code continues to use CUDPTask.
//
class NetworkPump {
public:
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

  // Abort all sessions (e.g. on Apple II reset). Sessions are
  // expected to clean up promptly when Abort() is called.
  void RequestAbortAll();

  // --- Helpers intended for future use by sessions ---

  udp_pcb *CreateUdpPcb(INetworkSession *owner, uint16_t local_port);
  void DestroyUdpPcb(udp_pcb *pcb);

  tcp_pcb *CreateTcpPcb(INetworkSession *owner);
  void DestroyTcpPcb(tcp_pcb *pcb);

  void ScheduleTimer(INetworkSession *owner, uint32_t timeout_ms, uint32_t arg);
  void CancelTimer(INetworkSession *owner);

private:
  // Internal bookkeeping structures will be filled in when we
  // migrate the first protocol (NTP/TFTP) to this pump.
  bool cyw43Inited;
  bool wifiConnected;
};

#endif // _NETWORK_PUMP_H


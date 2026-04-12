#include "network_pump.h"
#include "debug.h"
#include "misc.h"
#include <algorithm>
#include "lwip/pbuf.h"
#include "udptask.h"
#include "network.h"
#include "ntptask.h"
#include "testwifitask.h"
#include "tftprxtask.h"
#include "tftptxtask.h"
#include "tftpstate.h"

namespace {

struct ScopedRunSession {
  CUDPTask *t;
  explicit ScopedRunSession(CUDPTask *task) : t(task) { t->EnterRunSession(); }
  ~ScopedRunSession() { t->LeaveRunSession(); }
};

/// AddSession in ctor; RemoveSession in dtor if still armed (exception paths).
struct ScopedPumpSession {
  NetworkPump *pump;
  INetworkSession *session;
  bool armed;
  ScopedPumpSession(NetworkPump *p, INetworkSession *s) : pump(p), session(s), armed(true) {
    pump->AddSession(session);
  }
  void disarm() { armed = false; }
  ~ScopedPumpSession() {
    if (armed) pump->RemoveSession(session);
  }
};

/// Drives one legacy CUDPTask per PollOnce (same as one Run() loop iteration).
class LegacyUdpSessionAdapter : public INetworkSession {
public:
  explicit LegacyUdpSessionAdapter(CUDPTask *task) : task_(task) {}

  void OnPump(NetworkPump &) override {
    if (!task_) return;
    if (CUDPTask::GetRunningObject() != task_) return;
    if (!CUDPTask::IsRunning()) return;
    (void)task_->PumpNetworkIteration();
  }

private:
  CUDPTask *task_;
};

} // namespace

void INetworkSession::OnTcpRecvPbuf(struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
  (void)pcb;
  (void)err;
  if (p) {
    pbuf_free(p);
  }
}

static NetworkError_t MapUdpExceptionToTestWifiError(int e) {
  switch (e) {
    case CUDPTask::ERR_NOTPICOW: return NETERR_NOTPICOW;
    case CUDPTask::ERR_SSIDNOTSET: return NETERR_SSIDNOTSET;
    case CUDPTask::ERR_NONET: return NETERR_NONET;
    case CUDPTask::ERR_BADAUTH: return NETERR_BADAUTH;
    case CUDPTask::ERR_NOIP: return NETERR_NOIP;
    case CUDPTask::ERR_WIFINOTCONNECTED: return NETERR_WIFINOTCONNECTED;
    case CUDPTask::ERR_CONNECTIONLOST: return NETERR_WIFINOTCONNECTED;
    case CUDPTask::ERR_DNSINVALIDHOST: return NETERR_DNSFAILED;
    case CUDPTask::ERR_DNSTIMEOUT: return NETERR_DNSFAILED;
    case CUDPTask::ERR_WATCHDOG: return NETERR_TIMEOUT;
    case CUDPTask::ERR_ABORTED: return NETERR_ABORTED;
    case CNTPTask::ERR_NTPFAILED: return NETERR_NTPFAILED;
    default: return NETERR_UNKNOWN;
  }
}

NetworkPump::NetworkPump() :
  cyw43Inited(false),
  wifiConnected(false),
  activeLegacyOperation(LEGACY_OPERATION_NONE),
  activeLegacyTaskId(0),
  activeLegacyLabel(NULL),
  dns_pending_owner_(nullptr) {
}

NetworkPump::~NetworkPump() {
  sessions_.clear();
  udp_pcb_owners_.clear();
  tcp_pcb_owners_.clear();
  session_timers_.clear();
  dns_pending_owner_ = nullptr;
}

void NetworkPump::RegisterDnsPendingOwner(INetworkSession *owner) {
  dns_pending_owner_ = owner;
}

void NetworkPump::ClearDnsPendingOwner(INetworkSession *owner) {
  if (dns_pending_owner_ == owner) {
    dns_pending_owner_ = nullptr;
  }
}

void NetworkPump::RegisterUdpPcbOwner(udp_pcb *pcb, INetworkSession *owner) {
  if (!pcb || !owner) return;
  udp_pcb_owners_[pcb] = owner;
}

void NetworkPump::UnregisterUdpPcb(udp_pcb *pcb) {
  if (!pcb) return;
  udp_pcb_owners_.erase(pcb);
}

void NetworkPump::RegisterTcpPcbOwner(tcp_pcb *pcb, INetworkSession *owner) {
  if (!pcb || !owner) {
    return;
  }
  tcp_pcb_owners_[pcb] = owner;
}

void NetworkPump::UnregisterTcpPcb(tcp_pcb *pcb) {
  if (!pcb) {
    return;
  }
  tcp_pcb_owners_.erase(pcb);
}

void NetworkPump::dispatchLegacyUdpRecv(udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
  if (!p) return;
  if (!addr) {
    pbuf_free(p);
    return;
  }
  auto it = udp_pcb_owners_.find(pcb);
  if (it == udp_pcb_owners_.end() || !it->second) {
    WARN_PRINTF("NETPUMP: UDP recv for unregistered pcb 0x%p\n", (void *)pcb);
    pbuf_free(p);
    return;
  }
  it->second->OnUdpRecvPbuf(pcb, p, addr, (uint16_t)port);
  pbuf_free(p);
}

extern "C" void NetworkPump_LegacyUdpRecv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                          const ip_addr_t *addr, u16_t port) {
  if (!arg || !pcb) {
    if (p) pbuf_free(p);
    return;
  }
  static_cast<NetworkPump *>(arg)->dispatchLegacyUdpRecv(pcb, p, addr, port);
}

extern "C" err_t NetworkPump_LegacyTcpRecv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
  INetworkSession *session = static_cast<INetworkSession *>(arg);
  if (!session || !tpcb) {
    if (p) {
      pbuf_free(p);
    }
    return ERR_OK;
  }
  session->OnTcpRecvPbuf(tpcb, p, err);
  return ERR_OK;
}

extern "C" void NetworkPump_LegacyTcpErr(void *arg, err_t err) {
  INetworkSession *session = static_cast<INetworkSession *>(arg);
  if (session) {
    session->OnTcpErr(err);
  }
}

extern "C" void NetworkPump_LegacyDnsCallback(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
  (void)hostname;
  INetworkSession *session = static_cast<INetworkSession *>(arg);
  NetworkPump &pump = GetNetworkPump();
  // Ignore stale callbacks: superseded lookup, session ended, etc. (same rule as §14.9).
  if (session == nullptr || !pump.IsDnsPendingOwner(session)) {
    WARN_PRINTF("NETPUMP: DNS callback ignored (not pending owner)\n");
    return;
  }
  session->OnDnsGetHostByNameResult(ipaddr);
}

void NetworkPump::Init() {
  if (cyw43Inited) return;

  /* Same as CUDPTask::InitCyw43: misc InitPicoLed() may have inited CYW43. */
  if (cyw43_is_initialized(&cyw43_state)) {
    cyw43_arch_enable_sta_mode();
    cyw43Inited = true;
    return;
  }

  if (!cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE)) {
    cyw43_arch_enable_sta_mode();
    cyw43Inited = true;
  }
}

bool NetworkPump::EnsureWifiConnected(const char *ssid, const char *wpakey) {
  if (!cyw43Inited) {
    Init();
  }

  if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
    wifiConnected = true;
    return true;
  }

  if (!ssid || ssid[0] == '\0') {
    return false;
  }

  uint32_t authType;
  if (!wpakey || wpakey[0] == '\0') {
    authType = CYW43_AUTH_OPEN;
  } else {
    authType = CYW43_AUTH_WPA3_WPA2_AES_PSK;
  }

  absolute_time_t timeout = make_timeout_time_ms(15000);
  int err = cyw43_arch_wifi_connect_bssid_async(ssid, NULL, wpakey, authType);
  if (err) return false;

  int status = CYW43_LINK_UP + 1;
  while (status >= 0 && status != CYW43_LINK_UP) {
    int new_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (new_status != status) {
      status = new_status;
    }
    if (status < 0) break;
    if (time_reached(timeout)) break;
    cyw43_arch_poll();
    cyw43_arch_wait_for_work_until(timeout);
  }

  wifiConnected = (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP);
  return wifiConnected;
}

void NetworkPump::AddSession(INetworkSession *session) {
  if (!session) return;
  if (sessions_.size() >= MAX_SESSIONS) return;
  if (std::find(sessions_.begin(), sessions_.end(), session) != sessions_.end()) return;
  sessions_.push_back(session);
  session->OnStart(*this);
}

void NetworkPump::RemoveSession(INetworkSession *session) {
  if (!session) return;
  auto it = std::find(sessions_.begin(), sessions_.end(), session);
  if (it != sessions_.end()) sessions_.erase(it);
}

void NetworkPump::PollOnce() {
  cyw43_arch_poll();
  for (INetworkSession *s : sessions_) {
    if (s) s->OnPump(*this);
  }
  DrainSessionTimers();
}

void NetworkPump::DrainSessionTimers() {
  auto it = session_timers_.begin();
  while (it != session_timers_.end()) {
    if (time_reached(it->deadline)) {
      INetworkSession *s = it->session;
      uint32_t a = it->arg;
      it = session_timers_.erase(it);
      if (s) {
        s->OnTimer(a);
      }
    } else {
      ++it;
    }
  }
}

void NetworkPump::RequestAbortAll() {
  INFO_PRINTF("NETPUMP: RequestAbortAll()\n");
  if (activeLegacyOperation != LEGACY_OPERATION_NONE) {
    INFO_PRINTF("NETPUMP: aborting active legacy op=%d label=%s taskid=%u\n",
                (int)activeLegacyOperation,
                activeLegacyLabel ? activeLegacyLabel : "(null)",
                activeLegacyTaskId);
  }
  session_timers_.clear();
  for (INetworkSession *s : sessions_) {
    if (s) s->Abort();
  }
  UDPTask_RequestAbortIfRunning();
}

udp_pcb *NetworkPump::CreateUdpPcb(INetworkSession *owner, uint16_t local_port) {
  if (!owner) {
    return nullptr;
  }
  cyw43_arch_lwip_begin();
  udp_pcb *pcb = udp_new();
  if (!pcb) {
    WARN_PRINTF("NETPUMP: CreateUdpPcb udp_new failed\n");
    cyw43_arch_lwip_end();
    return nullptr;
  }
  err_t err = udp_bind(pcb, IP4_ADDR_ANY, local_port);
  if (err != ERR_OK) {
    WARN_PRINTF("NETPUMP: CreateUdpPcb udp_bind port=%u err=%d\n", (unsigned)local_port, (int)err);
    udp_remove(pcb);
    cyw43_arch_lwip_end();
    return nullptr;
  }
  RegisterUdpPcbOwner(pcb, owner);
  udp_recv(pcb, NetworkPump_LegacyUdpRecv, this);
  cyw43_arch_lwip_end();
  return pcb;
}

void NetworkPump::DestroyUdpPcb(udp_pcb *pcb) {
  if (!pcb) return;
  UnregisterUdpPcb(pcb);
  cyw43_arch_lwip_begin();
  udp_remove(pcb);
  cyw43_arch_lwip_end();
}

tcp_pcb *NetworkPump::CreateTcpPcb(INetworkSession *owner) {
  if (!owner) {
    return nullptr;
  }
  cyw43_arch_lwip_begin();
  tcp_pcb *pcb = tcp_new();
  if (!pcb) {
    WARN_PRINTF("NETPUMP: CreateTcpPcb tcp_new failed\n");
    cyw43_arch_lwip_end();
    return nullptr;
  }
  RegisterTcpPcbOwner(pcb, owner);
  tcp_arg(pcb, owner);
  tcp_recv(pcb, NetworkPump_LegacyTcpRecv);
  tcp_err(pcb, NetworkPump_LegacyTcpErr);
  cyw43_arch_lwip_end();
  return pcb;
}

void NetworkPump::DestroyTcpPcb(tcp_pcb *pcb) {
  if (!pcb) {
    return;
  }
  UnregisterTcpPcb(pcb);
  cyw43_arch_lwip_begin();
  tcp_arg(pcb, nullptr);
  tcp_recv(pcb, nullptr);
  tcp_err(pcb, nullptr);
  tcp_abort(pcb);
  cyw43_arch_lwip_end();
}

void NetworkPump::ScheduleTimer(INetworkSession *owner, uint32_t timeout_ms, uint32_t arg) {
  if (!owner) {
    return;
  }
  session_timers_.erase(std::remove_if(session_timers_.begin(), session_timers_.end(),
                                       [owner](const SessionTimer &t) { return t.session == owner; }),
                        session_timers_.end());
  SessionTimer st;
  st.session = owner;
  st.deadline = make_timeout_time_ms(timeout_ms);
  st.arg = arg;
  session_timers_.push_back(st);
}

void NetworkPump::CancelTimer(INetworkSession *owner) {
  if (!owner) {
    return;
  }
  session_timers_.erase(std::remove_if(session_timers_.begin(), session_timers_.end(),
                                       [owner](const SessionTimer &t) { return t.session == owner; }),
                        session_timers_.end());
}

void NetworkPump::BeginLegacyOperation(LegacyOperationKind kind, uint32_t taskid, const char *label) {
  activeLegacyOperation = kind;
  activeLegacyTaskId = taskid;
  activeLegacyLabel = label;
  DEBUG_PRINTF("NETPUMP: begin op=%d label=%s taskid=%u\n",
               (int)kind,
               label ? label : "(null)",
               taskid);
}

void NetworkPump::EndLegacyOperation() {
  if (activeLegacyOperation != LEGACY_OPERATION_NONE) {
    DEBUG_PRINTF("NETPUMP: end op=%d label=%s taskid=%u\n",
                 (int)activeLegacyOperation,
                 activeLegacyLabel ? activeLegacyLabel : "(null)",
                 activeLegacyTaskId);
  }
  activeLegacyOperation = LEGACY_OPERATION_NONE;
  activeLegacyTaskId = 0;
  activeLegacyLabel = NULL;
}

int32_t NetworkPump::RunTFTP(const uint32_t taskid,
                             const uint32_t dir,
                             const uint32_t unitNum,
                             const char *hostname,
                             const char *filename,
                             const bool enable1kBlockSize,
                             const uint32_t tftpTimeout,
                             const uint32_t tftpMaxAttempt,
                             const uint16_t tftpServerPort,
                             const char *ssid,
                             const char *wpakey) {
  BeginLegacyOperation(LEGACY_OPERATION_TFTP, taskid, "TFTP");

  int errorcode = 0;
  CTFTPRXTask *rxTask = NULL;
  CTFTPTXTask *txTask = NULL;
  CUDPTask *runTask = NULL;

  try {
    DEBUG_PRINTF("NETPUMP: RunTFTP dir=%u unit=%u host=%s file=%s\n",
                 dir, unitNum,
                 hostname ? hostname : "(null)",
                 filename ? filename : "(null)");
    /* CUDPTask + CTFTPTask ctor mallocs ~2.5 KiB; panic "Out of memory" is pico_malloc (NULL or past __StackLimit). */
    DebugPrintHeapState("NETPUMP: TFTP pre-new");
    if (dir==0) {
      rxTask = new CTFTPRXTask(unitNum, hostname, filename, enable1kBlockSize,
                               tftpTimeout, tftpMaxAttempt, tftpServerPort);
      runTask = rxTask;
    } else if (dir==1) {
      txTask = new CTFTPTXTask(unitNum, hostname, filename, enable1kBlockSize,
                               tftpTimeout, tftpMaxAttempt, tftpServerPort);
      runTask = txTask;
    } else {
      assert(0);
      ERROR_PRINTF("NETPUMP: invalid TFTP direction=%u\n", dir);
      EndLegacyOperation();
      return -99998;
    }
    ScopedRunSession scoped(runTask);
    LegacyUdpSessionAdapter adapter(runTask);
    runTask->BeginRun(ssid, wpakey);
    runTask->StartEventsAfterBeginRun();
    ScopedPumpSession sessionGuard(this, &adapter);
    while (!runTask->GetCompleted()) {
      PollOnce();
    }
    RemoveSession(&adapter);
    sessionGuard.disarm();
  } catch (int e) {
    ERROR_PRINTF("NETPUMP: TFTP task exception=%d\n", e);
    errorcode = e;
  } catch (...) {
    ERROR_PRINTF("NETPUMP: TFTP task unknown exception\n");
    errorcode = -99999;
  }

  if (rxTask) delete rxTask;
  if (txTask) delete txTask;

  EndLegacyOperation();
  return errorcode;
}

NetworkError_t NetworkPump::RunNTP(const char *ssid, const char *wpakey, time_t *out_seconds) {
  BeginLegacyOperation(LEGACY_OPERATION_NTP, 0, "NTP");
  NetworkError_t result = NETERR_NTPFAILED;
  CNTPTask task;
  ScopedRunSession scoped(&task);
  LegacyUdpSessionAdapter adapter(&task);
  try {
    task.BeginRun(ssid, wpakey);
    task.StartEventsAfterBeginRun();
    ScopedPumpSession sessionGuard(this, &adapter);
    while (!task.GetCompleted()) {
      PollOnce();
    }
    RemoveSession(&adapter);
    sessionGuard.disarm();
    if (out_seconds) {
      *out_seconds = task.GetSecondsSince1970();
    }
    result = NETERR_NONE;
  } catch (int e) {
    ERROR_PRINTF("NETPUMP: RunNTP exception=%d (%s)\n", e, CUDPTask::GetErrorCodeMessage(e));
    result = (e == CUDPTask::ERR_NOTPICOW) ? NETERR_NOTPICOW : NETERR_NTPFAILED;
  } catch (...) {
    DEBUG_PRINTF("NETPUMP: RunNTP unknown exception\n");
    result = NETERR_NTPFAILED;
  }
  EndLegacyOperation();
  return result;
}

void NetworkPump::RunTestWifi(TestResult_t *testResultPtr, const char *ssid, const char *wpakey) {
  BeginLegacyOperation(LEGACY_OPERATION_TESTWIFI, 0, "TestWifi");
  CTestWifiTask task(testResultPtr);
  ScopedRunSession scoped(&task);
  LegacyUdpSessionAdapter adapter(&task);
  try {
    task.BeginRun(ssid, wpakey);
    task.StartEventsAfterBeginRun();
    ScopedPumpSession sessionGuard(this, &adapter);
    while (!task.GetCompleted()) {
      PollOnce();
    }
    RemoveSession(&adapter);
    sessionGuard.disarm();
    if (task.GetCompleted()) {
      testResultPtr->error = NETERR_NONE;
      testResultPtr->testCompleted = true;
      EndLegacyOperation();
      return;
    }
  } catch (int e) {
    testResultPtr->error = MapUdpExceptionToTestWifiError(e);
    testResultPtr->testCompleted = true;
    EndLegacyOperation();
    return;
  } catch (...) {
    testResultPtr->error = NETERR_UNKNOWN;
    testResultPtr->testCompleted = true;
    EndLegacyOperation();
    return;
  }
  testResultPtr->error = NETERR_UNKNOWN;
  testResultPtr->testCompleted = true;
  EndLegacyOperation();
}


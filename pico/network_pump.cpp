#include "network_pump.h"
#include "debug.h"
#include "network.h"
#include "tftprxtask.h"
#include "tftptxtask.h"
#include "tftpstate.h"

NetworkPump::NetworkPump() :
  cyw43Inited(false),
  wifiConnected(false),
  activeLegacyOperation(LEGACY_OPERATION_NONE),
  activeLegacyTaskId(0),
  activeLegacyLabel(NULL) {
}

NetworkPump::~NetworkPump() {
  // For now we keep WiFi online across the process lifetime,
  // matching the current CUDPTask behaviour. When sessions
  // are introduced, we will add pcb cleanup here.
}

void NetworkPump::Init() {
  if (cyw43Inited) return;

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

void NetworkPump::AddSession(INetworkSession * /*session*/) {
  // Placeholder: will be implemented when migrating the first session.
}

void NetworkPump::RemoveSession(INetworkSession * /*session*/) {
  // Placeholder.
}

void NetworkPump::PollOnce() {
  // Placeholder: when sessions are wired up, this will:
  //  - call cyw43_arch_poll()
  //  - dispatch queued DNS/UDP/TCP events to sessions
  //  - run per-session timers/watchdogs.
  cyw43_arch_poll();
}

void NetworkPump::RequestAbortAll() {
  INFO_PRINTF("NETPUMP: RequestAbortAll()\n");
  if (activeLegacyOperation != LEGACY_OPERATION_NONE) {
    INFO_PRINTF("NETPUMP: aborting active legacy op=%d label=%s taskid=%u\n",
                (int)activeLegacyOperation,
                activeLegacyLabel ? activeLegacyLabel : "(null)",
                activeLegacyTaskId);
  }
  UDPTask_RequestAbortIfRunning();
}

udp_pcb *NetworkPump::CreateUdpPcb(INetworkSession * /*owner*/, uint16_t local_port) {
  cyw43_arch_lwip_begin();
  udp_pcb *pcb = udp_new();
  if (pcb) {
    udp_bind(pcb, IP4_ADDR_ANY, local_port);
  }
  cyw43_arch_lwip_end();
  return pcb;
}

void NetworkPump::DestroyUdpPcb(udp_pcb *pcb) {
  if (!pcb) return;
  cyw43_arch_lwip_begin();
  udp_remove(pcb);
  cyw43_arch_lwip_end();
}

tcp_pcb *NetworkPump::CreateTcpPcb(INetworkSession * /*owner*/) {
  cyw43_arch_lwip_begin();
  tcp_pcb *pcb = tcp_new();
  cyw43_arch_lwip_end();
  return pcb;
}

void NetworkPump::DestroyTcpPcb(tcp_pcb *pcb) {
  if (!pcb) return;
  cyw43_arch_lwip_begin();
  tcp_abort(pcb);
  cyw43_arch_lwip_end();
}

void NetworkPump::ScheduleTimer(INetworkSession * /*owner*/, uint32_t /*timeout_ms*/, uint32_t /*arg*/) {
  // Placeholder.
}

void NetworkPump::CancelTimer(INetworkSession * /*owner*/) {
  // Placeholder.
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
    runTask->Run(ssid, wpakey);
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


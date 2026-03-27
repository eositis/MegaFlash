
//////////////////////////////////////////////////////////////
//Entry point of network functions
//It is also the bridge between C and C++

#include "udptask.h"
#include "ntptask.h"
#include "testwifitask.h"
#include "tftprxtask.h"
#include "tftptxtask.h"
#include "userconfig.h"
#include "rtc.h"
#include "network.h"
#include "debug.h"
#include "network_pump.h"
#include "tftp.h"
#include <ctime>
#include <typeinfo>

static NetworkPump g_networkPump;

NetworkPump &GetNetworkPump() {
  return g_networkPump;
}

#ifdef __cplusplus
extern "C" {
#endif

//
//Export CUDPTask static method as C function
//
void UDPTask_RequestAbortIfRunning() {
  CUDPTask::RequestAbortIfRunning();
}

bool UDPTask_AbortTimeout_ms(const uint32_t timeout_ms) {
  return CUDPTask::AbortTimeout_ms(timeout_ms);
}

bool IsUDPTaskRunning() {
  return CUDPTask::IsRunning();
}

bool IsNTPTaskRunning() {
  CUDPTask *runningTask=CUDPTask::GetRunningObject();  
  if (runningTask==NULL) return false; //To avoid exception when dereference a NULL pointer below
  return typeid(CNTPTask)==typeid(*runningTask);
}

bool IsTestWifiTaskRunning() {
  CUDPTask *runningTask=CUDPTask::GetRunningObject();  
  if (runningTask==NULL) return false; //To avoid exception when dereference a NULL pointer below 
  return typeid(CTestWifiTask)==typeid(*runningTask);
}


bool IsTFTPTaskRunning() {
  CUDPTask *runningTask=CUDPTask::GetRunningObject();  
  return dynamic_cast<CTFTPTask*>(runningTask)!=nullptr;
}

//tftp_state defined in tftpstate.c
extern "C" volatile tftp_state_t tftp_state;

////////////////////////////////////////////////////////////
// Convert CUDPTask exception to TFTP ERROR
//
static int ConvertExceptionToTFTPError(int e) {
  switch(e) {
    case CUDPTask::ERR_NONE:
      return TFTPERROR_NOERR;
    case CUDPTask::ERR_NOTPICOW:
    case CUDPTask::ERR_SSIDNOTSET:
    case CUDPTask::ERR_NONET:
    case CUDPTask::ERR_BADAUTH:
    case CUDPTask::ERR_NOIP:
    case CUDPTask::ERR_WIFINOTCONNECTED:
      return TFTPERROR_WIFINOTCONNECTED;
    case CUDPTask::ERR_CONNECTIONLOST:
      return TFTPERROR_WIFICONNECTIONLOST;
    case CUDPTask::ERR_DNSINVALIDHOST:
    case CUDPTask::ERR_DNSTIMEOUT:
      return TFTPERROR_DNS;
    case CUDPTask::ERR_WATCHDOG:
      return TFTPERROR_WATCHDOG;
    case CUDPTask::ERR_ABORTED:
      return TFTPERROR_ABORTED;
    case CTFTPTask::ERR_RWFAILED:
      return TFTPERROR_RWFAILED;
    default:
      return TFTPERROR_UNKNOWN;
  }
}


void ExecuteTFTP(const uint32_t taskid) {
  uint32_t dir = tftp_state.dir;
  uint unitNum = tftp_state.unitNum;
  const char* hostname = (const char*)tftp_state.server_hostname;
  const char* filename = (const char*)tftp_state.filename;

  TRACE_PRINTF("ExecuteTFTP: taskid=%d\n",taskid);  
  TRACE_PRINTF("dir = %d\n",dir);
  TRACE_PRINTF("unitNum = %d\n",unitNum);
  TRACE_PRINTF("Hostname = %s\n",hostname);
  TRACE_PRINTF("filename = %s\n",filename);
  
  tftp_state.status = TFTPSTATUS_STARTING;  //make sure it is not COMPLETED. Otherwise, Apple may terminate the process immediately.
  tftp_state.startTime = get_absolute_time();
  tftp_state.taskid = taskid;
  TRACE_PRINTF("tftp_state.status = TFTPSTATUS_STARTING");
  
  //
  // Start the task
  //
  int errorcode = CUDPTask::ERR_NONE;
  const char* ssid = GetSSID();   
  const char* wpakey = GetWPAKey(); 

  errorcode = g_networkPump.RunTFTP(taskid, dir, unitNum, hostname, filename,
                                    GetTFTPEnable1kBlockSize(),
                                    GetTFTPTimeout(),
                                    GetTFTPMaxAttempt(),
                                    GetTFTPServerPort(),
                                    ssid, wpakey);

  if (errorcode != CUDPTask::ERR_NONE) {
    ERROR_PRINTF("NETPUMP: RunTFTP returned code=%d (%s)\n",
                 errorcode, CUDPTask::GetErrorCodeMessage(errorcode));
  }
  
  //Setup error code
  //Make sure status is set to COMPLETED
  tftp_critical_section_enter_blocking();
  if (errorcode!=CUDPTask::ERR_NONE) {
    tftp_state.error = ConvertExceptionToTFTPError(errorcode);
    ERROR_PRINTF("tftp_state.error = %d\n",tftp_state.error);
  }
  tftp_state.status = TFTPSTATUS_COMPLETED;
  tftp_critical_section_exit();
  
  //Free CTFTPTask object
}

void NetworkPump_RequestAbortAll() {
  g_networkPump.RequestAbortAll();
}

void NetworkPump_PollOnce(void) {
  g_networkPump.PollOnce();
}


////////////////////////////////////////////////////////////
//
//
NetworkError_t GetNetworkTime() {
  DEBUG_PRINTF("GetNetworkTime()\n");
  if (!GetNTPClientEnabled()) {
    return NETERR_NONE;
  }

  time_t epoch;
  NetworkError_t err = g_networkPump.RunNTP(GetSSID(), GetWPAKey(), &epoch);
  if (err == NETERR_NONE) {
    InitRTC(epoch, GetTimezoneOffset());
  }
  return err;
}



void TestWifi(TestResult_t *testResultPtr) {
  DEBUG_PRINTF("TestWifi()\n");
  g_networkPump.RunTestWifi(testResultPtr, GetSSID(), GetWPAKey());
}



#ifdef __cplusplus
}
#endif

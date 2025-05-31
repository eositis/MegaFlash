#ifndef _NETWORK_H
#define _NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pico/cyw43_arch.h"

//Definition of NetworkError_t
#include "../common/defines.h"

typedef struct {
  ip4_addr_t ipaddr;
  ip4_addr_t netmask;
  ip4_addr_t gateway;
  ip4_addr_t dnsserver;
  volatile int error;
  volatile bool testCompleted;
} TestResult_t;

void TestNetwork();

void UDPTask_RequestAbortIfRunning();
bool UDPTask_AbortTimeout_ms(const uint32_t timeout_ms);
bool IsUDPTaskRunning();
bool IsNTPTaskRunning();
bool IsTestWifiTaskRunning();
bool IsTFTPTaskRunning();


NetworkError_t GetNetworkTime();
void TestWifi(TestResult_t *testResultPtr);
void ExecuteTFTP(const uint32_t taskid);

#ifdef __cplusplus
}
#endif

#endif
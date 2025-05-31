extern "C" {
#include "lwip/dns.h"
}

#include "testwifitask.h"


void CTestWifiTask::EvtStart() {
  assert(testResultPtr);
  
  testResultPtr->ipaddr  = *netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
  testResultPtr->netmask = *netif_ip4_netmask(&cyw43_state.netif[CYW43_ITF_STA]);        
  testResultPtr->gateway = *netif_ip4_gw(&cyw43_state.netif[CYW43_ITF_STA]);
  testResultPtr->dnsserver = *dns_getserver(0);
  
  //Call base class implementation
  CNTPTask::EvtStart();
}
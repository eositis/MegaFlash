extern "C" {
#include "lwip/dns.h"
}

#include "testwifitask.h"


void CTestWifiTask::EvtStart() {
  assert(testResultPtr);
  
  DEBUG_PRINTF("TestWifi: EvtStart()\n");
  testResultPtr->ipaddr  = *netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
  testResultPtr->netmask = *netif_ip4_netmask(&cyw43_state.netif[CYW43_ITF_STA]);        
  testResultPtr->gateway = *netif_ip4_gw(&cyw43_state.netif[CYW43_ITF_STA]);
  testResultPtr->dnsserver = *dns_getserver(0);
  INFO_PRINTF("TestWifi: ip=%s gw=%s dns=%s\n",
              ip4addr_ntoa(&testResultPtr->ipaddr),
              ip4addr_ntoa(&testResultPtr->gateway),
              ip4addr_ntoa(&testResultPtr->dnsserver));
  
  //Call base class implementation
  CNTPTask::EvtStart();
}
#ifndef _IPC_H
#define _IPC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  IPCCMD_WIFITEST,
  IPCCMD_TFTP,
  IPCCMD_NET_WAKE /* Core 1 U2 bus activity → run U2_Net_Poll on core 0 sooner */
} IpcCmd;


struct IpcMsg {
  uint32_t command;
  uint32_t data;
};

#ifdef __cplusplus
}
#endif

#endif

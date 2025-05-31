#ifndef _IPC_H
#define _IPC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  IPCCMD_WIFITEST,
  IPCCMD_TFTP
} IpcCmd;


struct IpcMsg {
  uint32_t command;
  uint32_t data;
};

#ifdef __cplusplus
}
#endif

#endif

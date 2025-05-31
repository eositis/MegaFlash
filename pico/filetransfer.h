#ifndef _FILETRANSFER_H
#define _FILETRANSFER_H

#ifdef __cplusplus
extern "C" {
#endif

enum TxProtocol {
  XMODEM128,
  XMODEM1K
};

void Upload(const uint32_t unitnum);
void Download(const uint32_t unitNum, enum TxProtocol protocol);

#ifdef __cplusplus
}
#endif

#endif
#ifndef _ENCRYPTION_H
#define _ENCRYPTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void Decrypt(uint8_t* destBuffer,const uint8_t* srcBuffer, uint32_t len);
inline void Encrypt(uint8_t* destBuffer,const uint8_t* srcBuffer, uint32_t len) {
  Decrypt(destBuffer,srcBuffer,len);
}


#ifdef __cplusplus
}
#endif

#endif

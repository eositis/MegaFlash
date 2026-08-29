#ifndef MEGAFLASH_SMB_PRODOS_H
#define MEGAFLASH_SMB_PRODOS_H

#include <stdint.h>
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

void SmbProdos_Invalidate(void);
rwerror_t SmbProdos_ReadBlock(uint32_t blockNum, uint8_t *dest);
rwerror_t SmbProdos_WriteBlock(uint32_t blockNum, const uint8_t *src);
void SmbProdos_FillDibId(char *id16);

#ifdef __cplusplus
}
#endif

#endif

#ifndef MEGAFLASH_SMBDISK_H
#define MEGAFLASH_SMBDISK_H

#include <stdint.h>
#include <stdbool.h>
#include "defines.h"
#include "mediaaccess.h"

#ifdef __cplusplus
extern "C" {
#endif

void SmbDisk_Init(void);
void SmbDisk_OnConfigChanged(void);
uint32_t SmbDisk_GetUnitCount(void);
bool SmbDisk_Online(void);
uint32_t SmbDisk_GetBlockCount(void);
void SmbDisk_GetDIB(uint8_t *destBuffer);
rwerror_t SmbDisk_ReadBlock(uint32_t blockNum, uint8_t *destBuffer);
rwerror_t SmbDisk_WriteBlock(uint32_t blockNum, const uint8_t *srcBuffer);
void SmbDisk_GetStatusText(char *dest, uint32_t destlen);
void SmbDisk_ServiceCore0(void);

#ifdef __cplusplus
}
#endif

#endif

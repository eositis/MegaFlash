#ifndef _ROMDISK_H
#define _ROMDISK_H

#ifdef __cplusplus
extern "C" {
#endif

bool GetRomdiskEnabled();
void EnableRomdisk();
void DisableRomdisk();

uint32_t GetUnitCountRomdisk();
uint32_t GetBlockCountRomdisk();
uint32_t GetBlockCountRomdiskActual();
void GetDIBRomdisk(uint8_t *destBuffer);
rwerror_t ReadBlockRomdisk(const uint blockNum, uint8_t* destBuffer);

#ifdef __cplusplus
}
#endif

#endif
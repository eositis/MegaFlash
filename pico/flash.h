#ifndef _FLASH_H
#define _FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "defines.h"

//To store the location of a block
typedef struct {
  uint deviceNum;         //Flash Chip device number
  uint32_t blockAddress;  //Address of block
} blockloc_t;


void InitSpi();
void InitFlash();

uint32_t GetFlashSize();
uint32_t tsReadJEDECID(const uint deviceNum);
uint64_t tsReadUniqueID(const uint deviceNum);
uint64_t tsReadUniqueIDDevice0();

void tsEraseEverything();
void tsEraseSector64k(const uint deviceNum, uint32_t address);
bool tsIsSector64kErased(const uint deviceNum, uint32_t address);

void tsEraseSecurityRegister(const uint32_t regnum);
void tsReadSecurityRegister(const uint32_t regnum,uint8_t* dest,const uint8_t offset,const size_t len);
void tsWriteSecurityRegister(const uint32_t regnum,uint8_t* src,const uint8_t offset,const size_t len);

uint32_t GetUnitCountFlashActual();
uint32_t GetBlockCountFlash(const uint unitNum);
uint32_t GetBlockCountFlashActual(const uint unitNum);
blockloc_t GetBlockLoc(uint unitNum, const uint blockNum);
void GetDIBFlash(const uint unitNum, uint8_t *destBuffer);

//
// All ProDOS blocks must be handled by routines with _Public suffix.
// Read Bit Inversion note in flash.c file
//
rwerror_t tsReadBlockFlash_Public(const uint unitNum, const uint blockNum, uint8_t* destBuffer);
rwerror_t tsWriteBlockFlash_Public(const uint unitNum, const uint blockNum, const uint8_t* srcBuffer);
bool tsWriteOneBlockAlreadyErased_Public(const blockloc_t blockLoc, const uint8_t* srcBuffer);

//
// Erase Flash Disk
//
void tsEraseFlashDisk(const uint unitNum);
void AbortEraseFlashDisk();

#ifdef __cplusplus
}
#endif

#endif
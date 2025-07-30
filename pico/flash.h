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
uint32_t ReadJEDECID(const uint deviceNum);
uint64_t ReadUniqueID(const uint deviceNum);
uint64_t ReadUniqueIDDevice0();

void EraseEverything();
void EraseSector64k(const uint deviceNum, uint32_t address);
bool IsSector64kErased(const uint deviceNum, uint32_t address);

void EraseSecurityRegister(const uint32_t regnum);
void ReadSecurityRegister(const uint32_t regnum,uint8_t* dest,const uint8_t offset,const size_t len);
void WriteSecurityRegister(const uint32_t regnum,uint8_t* src,const uint8_t offset,const size_t len);


uint32_t GetUnitCountFlashActual();
uint32_t GetBlockCountFlash(const uint unitNum);
uint32_t GetBlockCountFlashActual(const uint unitNum);
blockloc_t GetBlockLoc(uint unitNum, const uint blockNum);
void GetDIBFlash(const uint unitNum, uint8_t *destBuffer);

//
// All ProDOS blocks must be handled by routines with _Public suffix.
// Read Bit Inversion note in flash.c file
//
rwerror_t ReadBlockFlash_Public(const uint unitNum, const uint blockNum, uint8_t* destBuffer);
rwerror_t WriteBlockFlash_Public(const uint unitNum, const uint blockNum, const uint8_t* srcBuffer);
bool WriteOneBlockAlreadyErased_Public(const blockloc_t blockLoc, const uint8_t* srcBuffer);

void EraseFlashDisk(const uint unitNum);


#ifdef __cplusplus
}
#endif

#endif
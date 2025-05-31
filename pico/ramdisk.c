#include "pico/stdlib.h"
#include <string.h>
#include "defines.h"
#include "dmamemops.h"
#include "mediaaccess.h"
#include "formatter.h"


//Smartport DIB ID String padded to 16 bytes long
#define IDSTR "RAMDISK         "
#define IDSTRLEN 7

//Volume name
#define VOLNAME "RAMDISK"
#define VOLNAMELEN 7

//RAMDisk Enable Flag
static bool ramdiskEnabled = false;

//RAMDisk data
static uint8_t __attribute__((aligned(4))) ramdisk_data[RAMDISK_SIZE];


///////////////////////////////////////
// Getter /Setter function
//
bool GetRamdiskEnabled() {
  return ramdiskEnabled;
}

void EnableRamdisk() {
  ramdiskEnabled = true;
}

void DisableRamdisk() {
  ramdiskEnabled = false;
}

uint32_t GetUnitCountRamdisk() {
  return ramdiskEnabled?1:0;
}

uint32_t GetBlockCountRamdisk() {
  return RAMDISK_SIZE/BLOCKSIZE;
}

uint32_t GetBlockCountRamdiskActual() {
  return RAMDISK_SIZE/BLOCKSIZE;
}

//Get RAMDisk data pointer
uint8_t* GetRamdiskDataPointer() {
  return ramdisk_data;
}

//Get RAMDisk Size in bytes
size_t GetRamdiskSize() {
  return RAMDISK_SIZE;
}

////////////////////////////////////////////////////////////////////
// Read Block from RAMDisk
// Assume blockNum is valid.
//
// Input: Block Number
//        destBuffer   Destination Buffer (512 Bytes)
//
// Output: SP_NOERR, SP_IOERR
//
rwerror_t ReadBlockRamdisk(const uint blockNum, uint8_t* destBuffer){
  //Validate Block Number
  if (blockNum >= GetBlockCountRamdisk()) return SP_IOERR;
  
  CopyMemoryAligned(destBuffer,ramdisk_data+blockNum*BLOCKSIZE,BLOCKSIZE);
  return SP_NOERR;
}

////////////////////////////////////////////////////////////////////
// Write Block to RAMDisk
// Assume blockNum is valid.
//
// Input: Block Number
//        srcBuffer   Source Buffer (512 Bytes)
//
// Output: SP_NOERR, SP_IOERR
//
rwerror_t WriteBlockRamdisk(const uint blockNum, const uint8_t* srcBuffer){
  //Validate Block Number
  if (blockNum >= GetBlockCountRamdisk()) return SP_IOERR;
  
  CopyMemoryAligned(ramdisk_data+blockNum*BLOCKSIZE,srcBuffer,BLOCKSIZE);
  return SP_NOERR;
}

/////////////////////////////////////////////////////////////
// Get DIB (Device Information Block) of a unit
//
// Input: destBuffer - Pointer to destination buffer
//        
void GetDIBRamdisk(uint8_t *destBuffer) {
  assert(sizeof(struct dib_t)==25);
  struct dib_t *dib = (struct dib_t*)destBuffer;  
  
  //Device Status Byte  
  dib->devicestatus = 0b11111000;          
  
  //Block Count
  uint32_t blockSize = GetBlockCountRamdisk();  
  dib->blocksize_l  = (uint8_t)blockSize; blockSize>>=8; 
  dib->blocksize_m  = (uint8_t)blockSize; blockSize>>=8;                    
  dib->blocksize_h  = (uint8_t)blockSize;
    
  //ID String    
  assert(strlen(IDSTR)==16);              
  assert(IDSTRLEN<=16);
  dib->idstrlen = IDSTRLEN;
  memcpy(dib->idstr,IDSTR,16);
  
  //Device Type, subtype and Firmware Version
  dib->devicetype = 0x00;                  //Device Type. $00 = RAM Disk
  dib->subtype = 0x20;                     //Subtype. $20= not removable, no extended call
  dib->fmversion_l = (uint8_t)FIRMWAREVER; //Firmware Version Word
  dib->fmversion_h = (uint8_t)(FIRMWAREVER>>8);
}

////////////////////////////////////////////////////////////////////
// Erase RAMDisk Quick
// Clear Block 0-2 so that the RAMDisk looks like unformatted
//
void EraseRamdiskQuick() {
  ZeroMemoryAligned(ramdisk_data,BLOCKSIZE*3); //Erase Block 0-2
}


////////////////////////////////////////////////////////////////////
// Erase entire RAMDisk
//
void EraseRamdisk() {
  ZeroMemoryAligned(ramdisk_data,RAMDISK_SIZE);
}


////////////////////////////////////////////////////////////////////
// Format RAMDisk
//
// Only first call is accepted. Subsequent call will be rejected
// to avoid erase of RAMDisk content
//
void FormatRamdiskOnce() {
  static bool formatted = false;
  
  if (formatted) return;    //Once only
  formatted = true;
  
  FormatUnit(GetRamdiskUnitNum(),GetBlockCountRamdisk(),VOLNAME,VOLNAMELEN);
  //Ignore error from FormatUnit(). Nothing we can do if error occurs
}


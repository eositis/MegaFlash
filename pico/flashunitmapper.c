#include <stdio.h>
#include <string.h>
#include "debug.h"
#include "flash.h"
#include "userconfig.h"
#include "misc.h"
#include "ramdisk.h"
#include "flashunitmapper.h"


/*******************************************************************
This module is to support Flash Drive Enable function. It sits between
mediaaccess.c and flash.c module to translate the unit number.

Example:
If there are 4 drives, only drive 2 and 4 are enabled.

The mapping is

 Logical Unit Number    Medium Unit Number
         1                       2
         2                       4

MapFlashUnitNum() function map the logical unit number to the medium
unit number. GetUnitCountFlashEnabled() function returns number
of units enabled.

Since the mapping is static, the mapping data is pre-calculated.
SetupFlashUnitMapping() function update the mapping data. It is called
at initialization and when enable flags are changed.
*********************************************************************/

//
// Maximum number of Flash Unit
//
// Only one byte is used to storge the enable flag. So, the number
// of flash unit is limited to 8.
// Also, ProDOS can support up to 14 drives. But some slots are reserved
// for other purpose, like slot 5/6 for floppy drives. So, max 8 drives
// is a practical limit.
//
#define MAXFLASHUNITCOUNT   8

//
//static Global Variables
//
static uint32_t unitCountFlashEnabled = 0;
static uint8_t unitNumberMap[MAXFLASHUNITCOUNT+1];
static bool mappingEnabled = false;    //Default is disabled

void EnableFlashUnitMapping() {
  mappingEnabled = true;
}

void DisableFlashUnitMapping() {
  mappingEnabled = false;
}



//////////////////////////////////////////////////////
// Setup the data for flash unit mapping
// It setups unitCountFlashEnabled variables
//
// Output: uintCount - Number of ProDOS drives
//
// Note: ((1<<actualCount)-1) is to generate the bitmask. e.g.
// If actualCount is 2, ((1<<actualCount)-1)=0b00000011.
// This bitmask is bitwise and with the enable flag to extract
// the first two bits.
void SetupFlashUnitMapping() { 
  //Assume Unit Count <=MAXFLASHUNITCOUNT
  assert(GetUnitCountFlashActual()<=MAXFLASHUNITCOUNT);
 
  //Get Number of unit actually exists
  uint32_t actualCount = MIN(MAXFLASHUNITCOUNT,GetUnitCountFlashActual());  
  
  //Mask out the enabled bits that does not actually exists
  uint32_t enableFlag = GetFlashdriveEnableFlag() & ((1<<actualCount)-1);  
  
  //Number of units enabled
  unitCountFlashEnabled = __builtin_popcount(enableFlag); //Number of bits set
  assert(unitCountFlashEnabled<=MAXFLASHUNITCOUNT);

  //
  //Setup unitNumberMap array
  //The index of the array is the logical unit number
  //The content of the array is the medium unit number
  //

  //Set all elements to 0
  memset(unitNumberMap, 0, sizeof(unitNumberMap));
  uint32_t curUnit=1;
  for(uint32_t i=1;i<=MAXFLASHUNITCOUNT;++i) {
    if (enableFlag & 0x01) {
      unitNumberMap[curUnit++] = i;
    }
    enableFlag>>=1;
  }
}


//////////////////////////////////////////////////////
// Return the unit count of Flash enabled.
//
// Output: uintCount - Number of ProDOS drives
//
uint32_t __no_inline_not_in_flash_func(GetUnitCountFlashEnabled)(){
  return mappingEnabled?unitCountFlashEnabled:GetUnitCountFlashActual();
}

////////////////////////////////////////////////////
// Map the logical unit number of flash drives to
// actual medium unit number
//
// Input: logicalUnitNum
//
// Output: mediumUnitNum
//
uint32_t __no_inline_not_in_flash_func(MapFlashUnitNum)(uint32_t logicalUnitNum) {
  //validate logicalUnitNum
  if (logicalUnitNum>MAXFLASHUNITCOUNT) {
    assert(false);
    return 0;   //Invalid logicalUnitNum. return 0 in release build
  }
  
  return mappingEnabled?unitNumberMap[logicalUnitNum]:logicalUnitNum;
}


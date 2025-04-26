#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "defines.h"
#include "ui-misc.h"
#include "ui-textinput.h"
#include "textstrings.h"
#include "asm.h"

/////////////////////////////////////////////////////////////////////
// Replace cgetc()
// Call DisplayTime() regularly while waiting for a key
//
// output: char - key pressed
//
char cgetc_showclock() {
  static_local uint16_t count;  
  
  do {
    if (count==0) DisplayTime();    
    ++count;
  }while(!kbhit());

  return cgetc();
}


///////////////////////////////////////////////////////////////////////
// Ask the user to type CONFIRM to confirm the operation
//
// Output: true  = User entered CONFIRM
//         false = Esc key pressed
//
bool AskUserToConfirm() {
  #define CONFIRM_LEN 7  //length of "CONFIRM" 
  static_local bool enter;
  

  cputs("Type CONFIRM to proceed:");
  ti_textBuffer[0] = '\0';
again:
  enter = ti_EnterText(CONFIRM_LEN,25,wherey(),-1); 
  if (enter==false) return false;    
  if (stricmp(ti_textBuffer,"CONFIRM")!=0) {
    beep();
    goto again;
  }
  
  return true;
}

///////////////////////////////////////////////////////////////////////
// Print Volume Type
// ProDOS, Empty or Unknown
//
// Input: type - Volume Type
//
void PrintVolumeType(uint8_t type) {
  if (type==TYPE_PRODOS) cputs(strProDOS);
  else if (type==TYPE_EMPTY) cputs(strEmpty);
  else cputs(strUnknown);
}

//////////////////////////////////////////////////////////////////
// Print information of all drives as a list
//
// Input: unitCount - Number of units
//
//Format:
//   012345678901234567890123456789012345
//   Drive Type   Volume Name     Blocks
//     1   ProDOS MEGAFLASH123456  65535
//     2   Unknown 
//
void PrintDriveInfoList(uint8_t unitCount) {
  static const char strHeader[] = "Drive Type   Volume Name     Blocks";
  static_local VolInfo_t volInfo;
  static_local uint8_t unit;
  
  cputs(strHeader);
  newline();
  for (unit =1;unit<=unitCount;++unit) {
   if (!GetVolInfo(unit,&volInfo)) FatalError(ERR_GETVOLINFO_FAIL);
   
   gotox(2);
   cprintf("%u",unit);
   
   gotox(6);
   PrintVolumeType(volInfo.type);
   
   if (volInfo.type==TYPE_PRODOS) {
     gotox(13);
     cputs(volInfo.volName);
   }

   gotox(30);
   cprintf("%5u",volInfo.blockCount); //right-justify
   newline();
  }
}
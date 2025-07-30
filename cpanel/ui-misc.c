#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <peekpoke.h>
#include "defines.h"
#include "ui-misc.h"
#include "ui-textinput.h"
#include "ui-wnd.h"
#include "textstrings.h"
#include "asm.h"

//Global Variable used by PrintDriveInfoList() and PrintDriveInfo()
static VolInfo_t volInfo;  //Data structure returned by CMD_GETVOLINFO command

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

  cputs("Type CONFIRM to proceed:");
  ti_textBuffer[0] = '\0';
again:
  if (!ti_EnterText(CONFIRM_LEN,25,wherey(),-1)) return false;    
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


//////////////////////////////////////////////////////////////////
// Print information of a drive
//
// Input: unit - Drive number
//
void PrintDriveInfo(uint8_t unit) {
  //Get current volume info
  if (!GetVolInfo(unit,&volInfo)) FatalError(ERR_GETVOLINFO_FAIL);  

  cputs(strCurrent);
  newline();
  cprintf(" Drive = %u",unit);  
  newline();
  cputs(" Type  = ");
  PrintVolumeType(volInfo.type);
  newline();
  if (volInfo.type==TYPE_PRODOS) {
    cprintf(strVolNameFormat,volInfo.volName);
    cprintf(strVolSizeFormat,volInfo.blockCount);
  }
}

//////////////////////////////////////////////////////////////////
// Print all flash drives as list for Drive Enable function
//
// Input: unitCount - Number of units
//
//Format:
//   01234567890123456789012345678901234
//   Drive Volume Name     Blocks Enable
//     1   MEGAFLASH123456  65535   ( )
//     2                            (X)
//              ....
//     8   --non-ProDOS--   655535  (X)
//     9   -- RAM Disk --     400   (X)
//
void PrintDriveList(uint8_t unitCount) {
  static const char strHeader[] = "Drive Volume Name     Blocks Enable";
  static const char strRamdisk[]  = "-- RAM Disk --";
  static const char strNonProdos[]= "--non-ProDOS--";
  static_local uint8_t unit;
  
  cputs(strHeader);
  newline();
  for (unit =1;unit<=unitCount;++unit) {
    if (!GetVolInfo(unit,&volInfo)) FatalError(ERR_GETVOLINFO_FAIL);
   
    //Drive Number  
    gotox(2);
    cprintf("%u",unit);
   
    //Volume Name
    gotox(6);
    if (volInfo.mediumType == TYPE_RAMDISK) cputs(strRamdisk);
    else if (volInfo.type != TYPE_PRODOS)   cputs(strNonProdos);
    else                                    cputs(volInfo.volName);

    //Block Count
    gotox(23);
    cprintf("%5u",volInfo.blockCount); //right-justify
    
    //Checkbox
    gotox(31);
    cprintf("( )");
    
    newline();
  }
}


//////////////////////////////////////////////////////////////////
// Print a string on two lines if it is too long.
//
// Input: width - width of the line
//        s     - the string
//
void PrintStringTwoLines(char* s,uint8_t width){
  uint8_t orgWndWidth = PEEK(WNDWDTH);
  POKE(WNDWDTH,width);
  cputs(s);
  POKE(WNDWDTH,orgWndWidth);
}

//////////////////////////////////////////////////////////////////
// print a string to screen up to num characters.
// If the string is longer than num, the last char is replaced with
// ellipsis.
//
// Input: s     - the string
//        num   - max number of characters to be printed
//
void cputs_n(char *s,uint8_t num) {
  static_local uint8_t i;
  static_local char c;
  static_local uint8_t num_minus1;
  num_minus1=num-1;
  
  for(i=0;i<num;++i) {
    c = s[i];
    if (c=='\0') return;
    if (i==num_minus1 && s[num]!='\0') c=ELLIPSIS;
    cputc(c);
  }
}  
  
//////////////////////////////////////////////////////
// Reset text screen to power on status
//  
void ResetScreen() {
  wnd_ResetScrollWindow();
  clrscr();
  setvid(); 
}  



#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include "textstrings.h"
#include "defines.h"
#include "ui-menu.h"
#include "ui-wnd.h"
#include "ui-textinput.h"
#include "ui-misc.h"
#include "asm.h"


//
// Defined in main.c
//
extern UserSettings_t config;



// Position and size of window
#define XPOS 1
#define YPOS 6
#define WIDTH 38
#define HEIGHT 16

//
//static Global Variables 
static bool enableFlags[9];  //Enable Flag of each drive
static bool ramdiskEnableFlag;
static bool romdiskEnableFlag;
static uint8_t i;           //Loop counter and array index


/////////////////////////////////////////////////////////////////////
// Draw Window Frame
//
static void DrawWindowFrame() {
  static const char windowTitle[]  = "Drives Enable";
  wnd_DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,windowTitle,true,true);  
}

///////////////////////////////////////////////////////////////////////
// Unpack fd_enableflags in config variable to enableFlags array
// and copy RAMDISKFLAG/ROMDISKFLAG from configbyte1 to ramdiskEnableFlag/romdiskEnableFlag
//
static void UnpackFlags() {
  static_local uint8_t flags; 
  
  flags = config.fd_enableflags;
  for(i=8;i!=0;--i) {
    enableFlags[i] = (flags & 0x80);
    flags <<=1;
  }
  ramdiskEnableFlag = (config.configbyte1 & RAMDISKFLAG);
  romdiskEnableFlag = (config.configbyte1 & ROMDISKFLAG);
}

/////////////////////////////////////////////////////////////////////
// Pack enableFlags array to fd_enableFlags in config variable and
// copy ramdiskEnableFlag/romdiskEnableFlag to configbyte1
//
static void PackFlags() { 
  config.fd_enableflags = enableFlags[8]?0x01:0x00;
  for(i=7;i!=0;--i) {
    config.fd_enableflags <<=1;
    if (enableFlags[i]) config.fd_enableflags |= 0x01;
  }  

  config.configbyte1 &= ~(RAMDISKFLAG | ROMDISKFLAG);
  if (ramdiskEnableFlag) config.configbyte1 |= RAMDISKFLAG;
  if (romdiskEnableFlag) config.configbyte1 |= ROMDISKFLAG;
}


////////////////////////////////////////////////////////////////////////
// Print or Clear the checkbox
//
// Input: index   - Index of checkbox (1-N)
//        checked - Checked or not
//
static void PrintCheckbox(uint8_t index,bool checked) {
  gotoxy(32,index);
  cputc(checked?'\304':' ');  //Tick or space
}


////////////////////////////////////////////////////////////////////
//  Main routine of Drives Enable function
//
void DoDrivesEnable() {
  static_local uint8_t listCount;
  static_local uint8_t unitCount;
  static_local unsigned char key;
  static_local bool newFlag;
  static_local uint8_t romdiskRow;
  
  unitCount = GetUnitCount();
  listCount = GetDriveListCount();  /* Excludes ROM disk when last (show on separate row) */
  UnpackFlags();
  
  //Draw window and its content (flash + RAM only; ROM disk has its own row below)
  DrawWindowFrame();
  PrintDriveListWithCheckboxes(listCount, enableFlags, ramdiskEnableFlag);
  
  //ROM Disk line - row after last drive. Header at YPOS, drive i at YPOS+i, so last at YPOS+listCount
  romdiskRow = YPOS + listCount + 1;
  gotoxy(2, romdiskRow);
  cputs("R");
  gotoxy(6, romdiskRow);
  cputs("\323\323\323ROM Disk\323\323\323");
  gotoxy(23, romdiskRow);
  cputs("  --");
  
  //ROM Disk checkbox (drives drawn inline in PrintDriveListWithCheckboxes)
  PrintCheckbox(romdiskRow, romdiskEnableFlag);
  
  //Prompt Messages (one newline to stay within 24-screen line limit)
  newline();
  cprintf(" 1-%d or R: toggle  Enter: OK", listCount);
  gotoxy(1,15);
  cputs(strCancelesc);
  gotox(31);
  cputs(strOK_Enter);
  
  //Event Loop
  do {
    key = cgetc_showclock();
    i = key - '0'; //Use i as array index variable
    
    //Toggle ROM Disk (key R or r)
    if (key == 'R' || key == 'r') {
      romdiskEnableFlag = !romdiskEnableFlag;
      PrintCheckbox(romdiskRow, romdiskEnableFlag);
      continue;
    }
    
    //Toggle RAMDisk
    if (i == listCount) {
      ramdiskEnableFlag = !ramdiskEnableFlag;
      PrintCheckbox(YPOS + listCount, ramdiskEnableFlag);
      continue;
    }
    
    //Toggle Flash drives
    if (i>=1 && i<listCount) {
      newFlag = !enableFlags[i];
      enableFlags[i] = newFlag;
      PrintCheckbox(YPOS + i, newFlag);
      continue;
    }
    
    //Enter Key
    if (key==KEY_ENTER) {
      //Pack the flags and save to config variable
      PackFlags();
      //Apply ROM disk visibility to Pico immediately
      if (romdiskEnableFlag) EnableRomdiskAtLast();
      else DisableRomdisk();
      break;    //exit the loop
    }
  }while (key != KEY_ESC);
}

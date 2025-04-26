#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"
#include "textstrings.h"
#include "ui-wnd.h"
#include "asm.h"
#include "config.h"
#include "mainmenu.h"

/******************************************************************
Build Types

Release: 
Output file is cpanel.bin. This file is included into Pico firmware.

Test Build:
Output file is cpanel.as (AppleSingle format). Then, this file is 
added to prodos19.dsk disk image for testing on emulator. Since 
Megaflash hardware does not exist on emulator, dummy test data are
used when the program tries to fetch information from Megaflash. 

MAME is used to test the program since it can emulate Apple IIc and
IIc+. It also support Slinky and serial port emulation.
******************************************************************/


//
//Global Variables
//
UserConfig_t config;
bool isAppleIIcplus;
bool isWifiSupported;
uint8_t boardType;
bool showPicoFirmwareVer;


/////////////////////////////////////////////////////////////////////
// Fatal Error handler
//
void FatalError(uint8_t errorcode) {
  wnd_ResetScrollWindow();
  clrscr();
  cprintf("Unexpected Error:%d\n\rPress any key to reboot.",errorcode);
  beep();
  cgetc();
  Reboot();
}



void main() { 
#ifndef TESTBUILD
  DisableROMDisk();
  boardType = GetBoardType();
  isWifiSupported = boardType & 0x80; //MSB set if Wifi is supported
#else
  boardType = BRD_PICOW;
  isWifiSupported = true;
#endif  

  isAppleIIcplus = IsAppleIIcplus();
  showPicoFirmwareVer = ReadOpenAppleButton(); 
  LoadConfig();
  DoMainMenu();
	
  wnd_ResetScrollWindow();
  clrscr();
	return;
}
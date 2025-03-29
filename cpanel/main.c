#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>

#include "defines.h"
#include "ui.h"
#include "wifi.h"
#include "asm.h"
#include "timezone.h"
#include "config.h"
#include "testwifi.h"
#include "format.h"

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
ConfigApple config;
static uint8_t key;

//Defined in ui.c
extern uint8_t currentMenuItem;

//Power up CPU speed option is for Apple IIc+ only
bool isAppleIIcplus;

uint8_t boardType;
bool isWifiSupported;

/////////////////////////////////////////////////////////////////////
// Fatal Error handler
//
void FatalError(uint8_t errorcode) {
  resetwndlft();
  clrscr();
  cprintf("Unexpected Error:%d\n",errorcode);
  beep();
  cgetc();
  Reboot();
}

const char strOKAnyKey[]="OK:Any Key";
const char strYes[]="(\304)";
const char strNo []="( )";
const char strConfirm[] = "Confirm";

char mmTitle[] = "MegaFlash Control Panel";
char mmPrompt[] ="Cancel:esc   Select:\310 \325 \312 \313 \315";
const char* mainMenuItems[] = {
  "Power on CPU Speed",
  "Auto-boot MegaFlash",
  "RAM Disk",
  "Applesoft BASIC FPU",
  "Network Time Sync",
  "Time Zone",
  "Wifi Setting >\n",
  "Test Wifi/NTP >",
  "Format >",
  "Erase All Settings\n",
  "Save and Reboot"
};
#define MMITEMCOUNT 11  
#define XPOS 4
#define YPOS 3
#define WIDTH 32
#define HEIGHT 18

void MoveCursorUpIfNotIIcplus() {
  if (!isAppleIIcplus) cursorup();  
}

void ShowNA() {
  gotox(XPOS+28);
  cputs("n/a");
}

void ShowCPUSpeed() {
  gotoxy(XPOS+25,YPOS+1);
  if (config.configbyte1 & CPUSPEEDFLAG) cputs("Normal");
  else cputs("  Fast");
}

void ToggleCPUSpeed() {
  config.configbyte1 = config.configbyte1 ^ CPUSPEEDFLAG;
  ShowCPUSpeed();
}

void ShowAutoBoot() {
  gotoxy(XPOS+28,YPOS+2);
  MoveCursorUpIfNotIIcplus();
  if (config.configbyte1 & AUTOBOOTFLAG) cputs(strYes);
  else cputs(strNo);
}

void ToggleAutoBoot() {
  config.configbyte1 = config.configbyte1 ^ AUTOBOOTFLAG;
  ShowAutoBoot();
}

void ShowRamdisk() {
  gotoxy(XPOS+28,YPOS+3);
  MoveCursorUpIfNotIIcplus();  
  if (config.configbyte1 & RAMDISKFLAG) cputs(strYes);
  else cputs(strNo);   
}

void ToggleRamdisk() {
  config.configbyte1 = config.configbyte1 ^ RAMDISKFLAG;
  ShowRamdisk();
}

void ShowFPU() {
  gotoxy(XPOS+28,YPOS+4);
  MoveCursorUpIfNotIIcplus();  
  if (config.configbyte1 & FPUFLAG) cputs(strYes);
  else cputs(strNo);   
}

void ToggleFPU() {
  config.configbyte1 = config.configbyte1 ^ FPUFLAG;
  ShowFPU();
}


 
void ShowNTPClient() {
  gotoxy(XPOS+28,YPOS+5);
  MoveCursorUpIfNotIIcplus();

  if (!isWifiSupported) {
    ShowNA();
    return;
  }  
  
  if (config.configbyte1 & NTPCLIENTFLAG) cputs(strYes);
  else cputs(strNo);  
}  
  
void ToggleNTPClient() {
  if (!isWifiSupported) return;  
  
  config.configbyte1 = config.configbyte1 ^ NTPCLIENTFLAG;
  ShowNTPClient();  
}
  
void ToggleTimezone(uint8_t key) {
  gotoxy(XPOS+22, YPOS+6);
  MoveCursorUpIfNotIIcplus();
  if (!isWifiSupported) {
    ShowNA();
    return;
  }    
  
  DoToggleTimezone(key);
}

void ShowTimezone() {
  ToggleTimezone(0);  //0 means refresh the screen without changing the time zone
}

void ShowClock() {
#ifndef TESTBUILD
    DisplayTime();
#endif
}  


void ShowAllOptions() {
  if (isAppleIIcplus) ShowCPUSpeed();
  ShowAutoBoot();
  ShowRamdisk();
  ShowFPU();
  ShowNTPClient();
  ShowTimezone(); 
}

void DrawMainMenuWindowFrame(bool isActive,bool clearContentArea) {
  DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,mmTitle,isActive,clearContentArea);
}


const char* yesnoMenuItem[] = {
  "Yes",
  "No"
};

//Return true if user select Yes
bool DoSaveConfirm() {
  #define SC_XPOS 10
  #define SC_YPOS 10
  #define SC_WIDTH 20
  #define SC_HEIGHT 6
  
  currentMenuItem=0;  //Default is Yes
  
  DrawWindow(SC_XPOS,SC_YPOS,SC_WIDTH,SC_HEIGHT,strConfirm,true,true);
  gotoxy(SC_XPOS+1,SC_YPOS);
  cputs("Save and Reboot?");
  
  do {
    key=DoMenu(yesnoMenuItem,2,SC_XPOS+3,SC_YPOS+2);
    
    if (key==KEY_ESC) return false;

  }while (key!=KEY_ENTER);
  
  if (currentMenuItem == 0) {
    DrawWindow(SC_XPOS,SC_YPOS,SC_WIDTH,SC_HEIGHT,strConfirm,true,true);
    gotoxy(SC_XPOS+1,SC_YPOS);
    cputs("Saving...");
  }

  //true if currentMenuItem==0
  return currentMenuItem^0x01;
}



//Erase all settings from Pico Flash Memory
void DoFactoryReset() {
  #define FRC_XPOS 5
  #define FRC_YPOS 9
  #define FRC_WIDTH 30
  #define FRC_HEIGHT 7
  
  currentMenuItem=1;  //Default is No
  
  DrawWindow(FRC_XPOS,FRC_YPOS,FRC_WIDTH,FRC_HEIGHT,strConfirm,true,true);
  gotoxy(FRC_XPOS+1,FRC_YPOS);
  cputs("Reset all options to default");
  gotoxy(FRC_XPOS+1,FRC_YPOS+1);  
  cputs("and erase WIFI setting?");

  do {
    key=DoMenu(yesnoMenuItem,2,FRC_XPOS+3,FRC_YPOS+3);
    
    if (key==KEY_ESC) return;

  }while (key!=KEY_ENTER);
  
  if (currentMenuItem == 0) {
    DrawWindow(FRC_XPOS,FRC_YPOS,FRC_WIDTH,FRC_HEIGHT,strConfirm,true,true);
    gotoxy(FRC_XPOS+1,FRC_YPOS);
    cputs("Erasing...");
    
    EraseAllConfig();
    LoadConfig();   //Reload Config
    
    cputs("Done!");  
    gotoxy(FRC_XPOS+19,FRC_YPOS+6);
    cputs(strOKAnyKey);
    cgetc();
  }
}


void ShowPicoWNeeded() {
  DrawWindow(5,9,30,5,"Error",true,true);
  gotoxy(6,9);
  cputs("Wifi requires PicoW Board.");
  gotoxy(24,13);
  cputs(strOKAnyKey);
  cgetc();
}


void DoMainMenu() {
  static_local bool redrawAll = true;
  static_local uint8_t itemCount;
  static_local uint8_t selectedItem;
  static_local const char** menuItems;
  
  if (isAppleIIcplus) {
    itemCount = MMITEMCOUNT;
    menuItems = mainMenuItems;
  }
  else {
    //Don't display the first item: CPU Speed
    itemCount = MMITEMCOUNT  -1; 
    menuItems = mainMenuItems+1;
  }
  currentMenuItem = 0;

  do{
    if (redrawAll) {
      redrawAll=false;
      clrscr();
      DrawMainMenuWindowFrame(true,false);
      gotoxy(XPOS+1,YPOS+17);
      cputs(mmPrompt);
      ShowAllOptions();
      ShowClock();
    }
    
    do {
      key=DoMenu(menuItems,itemCount,XPOS+1,YPOS+1);

      if (key==KEY_ESC) return;
      
      selectedItem = currentMenuItem;
      //On IIc, the first item is AutoBoot. So, add 1 to selectedItem      
      if (!isAppleIIcplus) ++selectedItem;

      switch (selectedItem) {
        case 0:
          ToggleCPUSpeed();
          break;
        case 1:
          ToggleAutoBoot();
          break;
        case 2:  
          ToggleRamdisk();
          break;
        case 3:
          ToggleFPU();
          break;
        case 4:
          ToggleNTPClient();
          break;
        case 5:
          //TimeZone
          ToggleTimezone(key);
          break;
        case 6:
          //Wifi Setting
          if (key!=KEY_ENTER) break;
          DrawMainMenuWindowFrame(false,false);  //Inactive Main Menu Window 
          redrawAll = true;        
          if (isWifiSupported) DoWifiSetting();
          else ShowPicoWNeeded();
          break;
        case 7:
          //Test Wifi/NTP
          //The Test window covers the main menu window completely.
          //No need to inactive main menu window 
          if (key!=KEY_ENTER) break;          
          redrawAll = true;    
          if (isWifiSupported) DoTestWifi();
          else ShowPicoWNeeded();
          break;
        case 8:
          //Format Megaflash Drive
          if (key!=KEY_ENTER) break;       
          DrawMainMenuWindowFrame(false,false);  //Inactive Main Menu Window           
          redrawAll = true;       
          DoFormat();
          break;
        case 9:
          //Erase All Settings
          if (key!=KEY_ENTER) break;     
          DrawMainMenuWindowFrame(false,false);  //Inactive Main Menu Window 
          redrawAll = true;
          DoFactoryReset();
          break;
        case 10:
          //Save and Reboot
          if (key!=KEY_ENTER) break;          
          DrawMainMenuWindowFrame(false,false);  //Inactive Main Menu Window 
          redrawAll = true;
          if (DoSaveConfirm()) {
           SaveConfig(); //No return. Reboot after saving
          } 
          break;
     }  
    }while(redrawAll==false);
  }while(1);
}

void main() { 
  
#ifndef TESTBUILD
  DisableROMDisk();
  isAppleIIcplus = IsAppleIIcplus();
  boardType = GetBoardType();
  isWifiSupported = boardType & 0x80; ;//MSB set if Wifi is supported
#else
  isAppleIIcplus = IsAppleIIcplus();
  boardType = BRD_PICOW;
  isWifiSupported = true;
#endif  
   
   
  LoadConfig();
  DoMainMenu();
	
	return;
}
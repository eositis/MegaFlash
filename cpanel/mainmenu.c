#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"
#include "dialogs.h"
#include "ui-menu.h"
#include "ui-wnd.h"
#include "config.h"
#include "asm.h"
#include "textstrings.h"
#include "mainmenu.h"
#include "timezone.h"
#include "wifi.h"
#include "testwifi.h"
#include "format.h"


//Variables
//=1 if running on IIc
//=0 if running on IIc+
//for adjusting postion of menu items
static uint8_t isAppleIIc;

//
// Defined in main.c
//
extern UserConfig_t config;
extern bool isAppleIIcplus;
extern bool isWifiSupported;
extern bool showPicoFirmwareVer;

//
// Constants
//
static const char strChecked[]    ="(\304)";
static const char strNotChecked[] ="( )";
static const char strNA []="n/a";

static char mmTitle[] = "MegaFlash Control Panel";
static char mmPrompt[] ="Cancel:esc   Select:\310 \325 \312 \313 \315";
static const char* mainMenuItems[] = {
  "Power on CPU Speed",
  "Auto-boot MegaFlash",
  "RAM Disk",
  "Applesoft BASIC FPU",
  "Network Time Sync",
  "Time Zone",
  "Wifi Setting >\n",
  "Test Wifi/NTP >",
  "Image Transfer via WIFI >",
  "Format >",
  "Erase All Settings\n",
  "Save and Reboot"
};
#define MMITEMCOUNT 12
#define MM_XPOS 4
#define MM_YPOS 3
#define MM_WIDTH 32
#define MM_HEIGHT 18


static void ShowNA() {
  gotox(27);
  cputs(strNA);
}

static void ShowCPUSpeed() {
  gotoxy(24,1);
  if (config.configbyte1 & CPUSPEEDFLAG) cputs("Normal");
  else cputs("  Fast");
}

static void ToggleCPUSpeed() {
  config.configbyte1 = config.configbyte1 ^ CPUSPEEDFLAG;
  ShowCPUSpeed();
}

static void ShowAutoBoot() {
  gotoxy(27,2-isAppleIIc);
  if (config.configbyte1 & AUTOBOOTFLAG) cputs(strChecked);
  else cputs(strNotChecked);
}

static void ToggleAutoBoot() {
  config.configbyte1 = config.configbyte1 ^ AUTOBOOTFLAG;
  ShowAutoBoot();
}

static void ShowRamdisk() {
  gotoxy(27,3-isAppleIIc);
  if (config.configbyte1 & RAMDISKFLAG) cputs(strChecked);
  else cputs(strNotChecked);   
}

static void ToggleRamdisk() {
  config.configbyte1 = config.configbyte1 ^ RAMDISKFLAG;
  ShowRamdisk();
}

static void ShowFPU() {
  gotoxy(27,4-isAppleIIc);
  if (HasFPUSupport()) {
    if (config.configbyte1 & FPUFLAG) cputs(strChecked);
    else cputs(strNotChecked);   
  } else {
    cputs(strNA);
  }
}

static void ToggleFPU() {
  if (HasFPUSupport()) {
    config.configbyte1 = config.configbyte1 ^ FPUFLAG;
    ShowFPU();
  }
}

static void ShowNTPClient() {
  gotoxy(27,5-isAppleIIc);

  if (!isWifiSupported) {
    ShowNA();
  } else {
    if (config.configbyte1 & NTPCLIENTFLAG) cputs(strChecked);
    else cputs(strNotChecked);  
  }
}  
  
static void ToggleNTPClient() {
  if (!isWifiSupported) return;  
  
  config.configbyte1 = config.configbyte1 ^ NTPCLIENTFLAG;
  ShowNTPClient();  
}
  
static void ToggleTimezone(uint8_t key) {
  gotoxy(21,6-isAppleIIc);
  if (!isWifiSupported) {
    ShowNA();
  } else {
    DoToggleTimezone(key);  
  }
}

static void ShowTimezone() {
  ToggleTimezone(0);  //0 means refresh the screen without changing the time zone
}

static void ShowAllOptions() {
  if (isAppleIIcplus) ShowCPUSpeed();
  ShowAutoBoot();
  ShowRamdisk();
  ShowFPU();
  ShowNTPClient();
  ShowTimezone(); 
}

static void DrawMainMenuWindowFrame(bool isActive) {
  wnd_DrawWindow(MM_XPOS,MM_YPOS,MM_WIDTH,MM_HEIGHT,mmTitle,isActive,false);
}


static void ShowFirmwareVer() {
  gotoxy(0,23);
  cputs("Pico Firmware ");
  SendCommand(CMD_GETDEVINFO);
  PrintStringFromDataBuffer();
}

void DoMainMenu() {
  static_local uint8_t itemCount;
  static_local uint8_t selectedItem;
  static_local const char** menuItems;
  static_local bool redrawAll = true;
  uint8_t key;    //Making it static_local results in longer code
  
  if (isAppleIIcplus) {
    itemCount = MMITEMCOUNT;
    menuItems = mainMenuItems;
    isAppleIIc = 0;
  }
  else {
    //Don't display the first item on IIc: CPU Speed
    itemCount = MMITEMCOUNT  -1; 
    menuItems = mainMenuItems+1;
    isAppleIIc = 1;   //Move some items up
  }
  
  mnu_currentMenuItem = 0;
  do{
    if (redrawAll) {
      redrawAll=false;
      wnd_ResetScrollWindow();
      clrscr();
      //The firmware is drawn outside the Main Menu Window
      //show it while scroll window is reset.
      if (showPicoFirmwareVer) ShowFirmwareVer();

      DrawMainMenuWindowFrame(true);
      gotoxy(0,17);
      cputs(mmPrompt);
      ShowAllOptions();
      DisplayTime();
    }
    
    do {
      key=DoMenu(menuItems,itemCount,0,1);
      if (key==KEY_ESC) return;
      
      selectedItem = mnu_currentMenuItem;
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
          DrawMainMenuWindowFrame(false);  //Inactive Main Menu Window 
          redrawAll = true;        
          if (isWifiSupported) DoWifiSetting();
          else ShowPicoWNeededDialog();
          break;
        case 7:
          //Test Wifi/NTP
          //The Test window covers the main menu window completely.
          //No need to inactive main menu window 
          if (key!=KEY_ENTER) break;          
          redrawAll = true;    
          if (isWifiSupported) DoTestWifi();
          else ShowPicoWNeededDialog();
          break;
        case 8:  
          //Disk Image Transfer
          //DoImageTransfer();
          beep();       //Not yet implemented
          //redrawAll = true;   
          break;
        case 9:
          //Format Megaflash Drive
          if (key!=KEY_ENTER) break;       
          DrawMainMenuWindowFrame(false);  //Inactive Main Menu Window           
          redrawAll = true;       
          DoFormat();
          break;
        case 10:
          //Erase All Settings
          if (key!=KEY_ENTER) break;     
          DrawMainMenuWindowFrame(false);  //Inactive Main Menu Window 
          redrawAll = true;
          ShowEraseSettingsDialog();
          break;
        case 11:
          //Save and Reboot
          if (key!=KEY_ENTER) break;          
          DrawMainMenuWindowFrame(false);  //Inactive Main Menu Window 
          redrawAll = true;
          if (ShowSaveConfirmDialog()) {
           SaveConfigReboot(); //No return. Reboot after saving
          } 
          break;
     }  
    }while(redrawAll==false);
  }while(1);
}

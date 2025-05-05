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
#include "tftp.h"


//Variables
//=1 if running on IIc
//=0 if running on IIc+
//for adjusting position of menu items
static uint8_t isAppleIIc;

//
// Defined in main.c
//
extern UserSettings_t config;
extern bool isAppleIIcplus;
extern bool isWifiSupported;

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
  "Boot MegaFlash",
  "RAM Disk",
  "Applesoft BASIC FPU",
  "Network Time Sync",
  "Time Zone",
  "Wifi Settings >\n",
  "Test Wifi/NTP >",
  "Disk Image Transfer via WIFI >",
  "Format >",
  "Erase All Settings\n",
  "Save and Reboot"
};
#define MMITEMCOUNT 12

//Window Position and Size
#define XPOS 4
#define YPOS 3
#define WIDTH 32
#define HEIGHT 18

//Menu Position
#define MENU_XPOS 0
#define MENU_YPOS 0


static void ShowNA() {
  gotox(27);
  cputs(strNA);
}

static void ShowCPUSpeed() {
  gotoxy(24,MENU_YPOS);
  if (config.configbyte1 & CPUSPEEDFLAG) cputs("Normal");
  else cputs("  Fast");
}

static void ToggleCPUSpeed() {
  config.configbyte1 = config.configbyte1 ^ CPUSPEEDFLAG;
  ShowCPUSpeed();
}

static void ShowAutoBoot() {
  gotoxy(27,MENU_YPOS+1-isAppleIIc);
  if (config.configbyte1 & AUTOBOOTFLAG) cputs(strChecked);
  else cputs(strNotChecked);
}

static void ToggleAutoBoot() {
  config.configbyte1 = config.configbyte1 ^ AUTOBOOTFLAG;
  ShowAutoBoot();
}

static void ShowRamdisk() {
  gotoxy(27,MENU_YPOS+2-isAppleIIc);
  if (config.configbyte1 & RAMDISKFLAG) cputs(strChecked);
  else cputs(strNotChecked);   
}

static void ToggleRamdisk() {
  config.configbyte1 = config.configbyte1 ^ RAMDISKFLAG;
  ShowRamdisk();
}

static void ShowFPU() {
  gotoxy(27,MENU_YPOS+3-isAppleIIc);
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
  gotoxy(27,MENU_YPOS+4-isAppleIIc);

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
  gotoxy(21,MENU_YPOS+5-isAppleIIc);
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
  wnd_DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,mmTitle,isActive,false);
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
  
  selectedItem = 0;
  do{
    if (redrawAll) {
      redrawAll=false;
      wnd_ResetScrollWindow();
      clrscr();
      DrawMainMenuWindowFrame(true);
      gotoxy(0,17);
      cputs(mmPrompt);
      ShowAllOptions();
      DisplayTime();
    }
    
    do {
      mnu_currentMenuItem = selectedItem; //Restore last selected item
      key=DoMenu(menuItems,itemCount,MENU_XPOS,MENU_YPOS);
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
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          redrawAll = true;        
          if (isWifiSupported) DoWifiSetting();
          else ShowPicoWNeededDialog();
          break;
        case 7:
          //Test Wifi/NTP
          //The Test window covers the main menu window completely.
          //No need to Inactivate main menu window 
          if (key!=KEY_ENTER) break;          
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window                    
          redrawAll = true;    
          if (isWifiSupported) DoTestWifi();
          else ShowPicoWNeededDialog();
          break;
        case 8:  
          //Disk Image Transfer
          if (key!=KEY_ENTER) break;             
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          DoTFTPImageTransfer();
          redrawAll = true;   
          break;
        case 9:
          //Format Megaflash Drive
          if (key!=KEY_ENTER) break;       
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window           
          redrawAll = true;       
          DoFormat();
          break;
        case 10:
          //Erase All Settings
          if (key!=KEY_ENTER) break;     
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          redrawAll = true;
          ShowEraseSettingsDialog();
          break;
        case 11:
          //Save and Reboot
          if (key!=KEY_ENTER) break;          
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          redrawAll = true;
          if (ShowSaveConfirmDialog()) {
           SaveUserSettingsReboot(); //No return. Reboot after saving
          } 
          break;
     }  
    }while(redrawAll==false);
  }while(1);
}

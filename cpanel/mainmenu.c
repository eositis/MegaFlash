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
#include "drivesenable.h"


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

enum COMMANDS {
  ID_POWERONSPEED,
  ID_BOOTMF,
  ID_BOOTROMDISK,
  ID_FPU,
  ID_NTPCLIENT,
  ID_TIMEZONE,
  ID_DRIVESENABLE,
  ID_WIFISETTINGS,
  ID_TESTWIFI,
  ID_TFTP,
  ID_FORMAT,
  ID_ERASESETTINGS,
  ID_SAVEANDREBOOT
};

static char* mainMenuItems[] = {
  "Power on CPU Speed",
  "Boot MegaFlash",
  "Boot to ROM Disk",
  "Applesoft BASIC FPU",  
  "Network Time Sync",
  "Time Zone",
  "Drives Enable >\0",//When Wifi Settings is removed, we need to move the /n char to this item. \0 is the placeholder of the /n char
  "Wifi Settings >\n",
  
  "Test Wifi/NTP >",
  "Disk Image Transfer via WIFI >",
  "Format >",
  "Erase All Settings\n",
  
  "Save and Reboot"
};
static uint8_t mainMenuIDs[] = {
  ID_POWERONSPEED,
  ID_BOOTMF,
  ID_BOOTROMDISK,
  ID_FPU,
  ID_NTPCLIENT,
  ID_TIMEZONE,
  ID_DRIVESENABLE,
  ID_WIFISETTINGS,
  ID_TESTWIFI,
  ID_TFTP,
  ID_FORMAT,
  ID_ERASESETTINGS,
  ID_SAVEANDREBOOT
};
#define MMITEMCOUNT (sizeof(mainMenuItems)/sizeof((mainMenuItems)[0]))
static uint8_t mainMenuItemCount = MMITEMCOUNT;


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
  gotox(24);
  if (config.configbyte1 & CPUSPEEDFLAG) cputs("Normal");
  else cputs("  Fast");
}

static void ToggleCPUSpeed() {
  config.configbyte1 = config.configbyte1 ^ CPUSPEEDFLAG;
  ShowCPUSpeed();
}

static void ShowAutoBoot() {
  gotox(27);
  if (config.configbyte1 & AUTOBOOTFLAG) cputs(strChecked);
  else cputs(strNotChecked);
}

static void ToggleAutoBoot() {
  config.configbyte1 = config.configbyte1 ^ AUTOBOOTFLAG;
  ShowAutoBoot();
}

static void ShowFPU() {
  gotox(27);
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
  gotox(27);

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
  gotox(21);
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
  //The order must be the same as mainMenuItems
  gotoxy(0,MENU_YPOS);
  if (isAppleIIcplus) {
    ShowCPUSpeed(); cursordown();
  }
  ShowAutoBoot();  cursordown();
  ShowFPU();       cursordown();
  if (isWifiSupported) {
    ShowNTPClient(); cursordown();
    ShowTimezone();
  }
}

static void DrawMainMenuWindowFrame(bool isActive) {
  wnd_DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,mmTitle,isActive,false);
}

///////////////////////////////////////////////////////////
// To remove an item from menu
//
// Input: uint8_t item - The array index of the item to be removed.
//
// Note: Remove menu items in reverse order. ie. from the highest 
// array index to 0. Then, we can use the ID_XXXX constants to identify
// the item.
static void RemoveMenuItem(uint8_t item) {
  uint8_t i;  //change to static_local does not reduce code size
  
  //Use MMITEMCOUNT constant instead of mainMenuItemCount
  //to reduce code size
  for(i=item;i<MMITEMCOUNT-1;++i) {
    mainMenuItems[i] = mainMenuItems[i+1];
    mainMenuIDs[i] = mainMenuIDs[i+1];
  }
  --mainMenuItemCount;
}

void DoMainMenu() {
  static_local bool redrawAll = true;
  static_local uint8_t key; 
  static_local uint8_t selectedItem;

  //
  //Remove unwanted menu items. 
  //The order is from highest ID number to 0
  if (!isWifiSupported) {
    RemoveMenuItem(ID_TFTP);
    RemoveMenuItem(ID_TESTWIFI);
    mainMenuItems[ID_DRIVESENABLE][strlen(mainMenuItems[ID_DRIVESENABLE])]='\n'; //move the new line char from WIFI Settings to FPU
    RemoveMenuItem(ID_WIFISETTINGS);
    RemoveMenuItem(ID_TIMEZONE);
    RemoveMenuItem(ID_NTPCLIENT);
  }
  if (!isAppleIIcplus) {
    RemoveMenuItem(ID_POWERONSPEED);
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
      //Restore current item
      mnu_currentMenuItem = selectedItem;

      key=DoMenu((const char**)mainMenuItems,mainMenuItemCount,MENU_XPOS,MENU_YPOS);
      if (key==KEY_ESC) return;

      //Save current Item since mnu_currentMenuItem may be changed by
      //command handler such as ShowEraseSettingsDialog()
      selectedItem = mnu_currentMenuItem;

      //Move the cursor to selected item
      gotoy(selectedItem+MENU_YPOS);

      //Convert selected menu item to ID
      switch (mainMenuIDs[selectedItem]) {
        case ID_POWERONSPEED:
          ToggleCPUSpeed();
          break;
        case ID_BOOTMF:
          ToggleAutoBoot();
          break;
        case ID_BOOTROMDISK:
          if (key != KEY_ENTER) break;
          BootToRomdisk();  /* no return */
          break;
        case ID_FPU:
          ToggleFPU();
          break;
        case ID_NTPCLIENT:
          ToggleNTPClient();
          break;
        case ID_TIMEZONE:
          ToggleTimezone(key);
          break;
        case ID_DRIVESENABLE:
          if (key!=KEY_ENTER) break;     
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          redrawAll = true;
          DoDrivesEnable();
          break;          
        case ID_WIFISETTINGS:
          if (key!=KEY_ENTER) break;
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          redrawAll = true;        
          if (isWifiSupported) DoWifiSetting();
          else ShowPicoWNeededDialog();
          break;
        case ID_TESTWIFI:
          //The Test window covers the main menu window completely.
          //No need to Inactivate main menu window 
          if (key!=KEY_ENTER) break;          
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window                    
          redrawAll = true;    
          if (isWifiSupported) DoTestWifi();
          else ShowPicoWNeededDialog();
          break;
        case ID_TFTP:  
          if (key!=KEY_ENTER) break;             
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          DoTFTPImageTransfer();
          redrawAll = true;   
          break;
        case ID_FORMAT:
          //Format Megaflash Drive
          if (key!=KEY_ENTER) break;       
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window           
          redrawAll = true;       
          DoFormat();
          break;
        case ID_ERASESETTINGS:
          if (key!=KEY_ENTER) break;     
          DrawMainMenuWindowFrame(false);  //Inactivate Main Menu Window 
          redrawAll = true;
          ShowEraseSettingsDialog();
          break;
        case ID_SAVEANDREBOOT:
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

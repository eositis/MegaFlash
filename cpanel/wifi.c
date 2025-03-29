#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>

#include "defines.h"
#include "ui.h"
#include "asm.h"

// Position and size of WIFI setting window
#define XPOS 1
#define YPOS 6
#define WIDTH 38
#define HEIGHT 15


char strEditPrompt [] = "Cancel:esc   \310\323:delete     Next:\315";

char wifiTitle[]    = "WIFI Setting (1/2)";
char wifiPrompt1[]  = "For security, current WIFI setting\n\rcannot be retrieved.";
char wifiPrompt2[]  = "Enter new setting to proceed.";
char wifiPrompt3[]  = "Network Name (SSID):";
char wifiPrompt4 [] = "(        . case sensitive)";

/////////////////////////////////////////////////////////////////////
// Draw WIFI Setting Window Frame
// 
// Input: page - '1' or '2' for page number display
//        isActive - The window is active
//        clearContentArea - To clear the content area
//
void DrawWifiWindowFrame(char page,bool isActive,bool clearContentArea) {
  wifiTitle[14]=page;  
  DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,wifiTitle,isActive,clearContentArea);  
}

/////////////////////////////////////////////////////////////////////
// Draw WIFI Setting Window Page 1
// 
void DrawWifiWindowPage1() {
  
  DrawWifiWindowFrame('1',true,true);
  setwndlft(XPOS+1);

  gotoxy(0,YPOS);
  cputs(wifiPrompt1);
  newline2();
  cputs(wifiPrompt2);  
  
  newline2();
  cputs(wifiPrompt3);
  gotoxy(1,YPOS+HEIGHT-1);
  cputs(strEditPrompt);  

  gotoxy(0,YPOS+8);
  cputs(wifiPrompt4);
  gotox(1);
  cputs("required");
  
  resetwndlft();
}

char wifiPrompt6[] = "WPA2/WPA3 AES Personal (PSK)\n\rauthentication is used to connect\n\rto WIFI network.";
char wifiPrompt7[] = "To connect to an open network,\n\rleave the Passphase blank.";
char wifiPrompt8[] = "WIFI Passphase (WPA Pre-Shared Key):";

/////////////////////////////////////////////////////////////////////
// Draw WIFI Setting Window Page 1
// 
void DrawWifiWindowPage2() {
  DrawWifiWindowFrame('2',true,true);
  setwndlft(XPOS+1);
  
  gotoxy(0, YPOS);
  cputs(wifiPrompt6);
  
  newline2();
  cputs(wifiPrompt7);

  newline2();
  cputs(wifiPrompt8);
  
  gotoxy(1,YPOS+HEIGHT-1);  
  cputs(strEditPrompt);  
  gotox(28);
  cputs("Save");  
  
  gotoxy(0,YPOS+11);
  cputs(wifiPrompt4);
  gotox(1);
  cputs("optional");
  
  resetwndlft();
}

//position and size of the window which shows
//the user the setting has been saved
#define XPOS2   3
#define YPOS2   5
#define WIDTH2  34
#define HEIGHT2 11
char savedPrompt1[]="New WIFI Setting has been saved.";
char savedPrompt2[]="Network Name:";
char savedPrompt3[]="WIFI Password:";
extern const char strOKAnyKey[];


/////////////////////////////////////////////////////////////////////
// Draw WIFI Saved window frame
// 
void DrawWifiSavedWindowFrame() {
  DrawWindow(XPOS2,YPOS2,WIDTH2,HEIGHT2,"WIFI Setting",true,true);  
  gotoxy(XPOS2+1,YPOS2);
  cputs("Saving...");
}

/////////////////////////////////////////////////////////////////////
// Draw the content WIFI Saved window
//
// Input ssid,password - New WIFI setting
//       success - Saving of setting is successful
// 
void DrawWifiSavedWindowContent(char* ssid,char* password,bool success) {
  char c; //Making it static_local results in longer code
  
  setwndlft(XPOS2+1);
  
  if (success) {
    gotoxy(0,YPOS2);    
    cputs(savedPrompt1);
    newline2();
    cputs(savedPrompt2);
    newline2();
    cputs(ssid);
    newline2();
    cputs(savedPrompt3);
    
    newline();
    if (strlen(password)<33) cputs(password);
    else {
      //Two lines are required to display the password
      c=password[32];
      password[32]='\0';
      cputs(password);
      password[32]=c;
      newline();
      cputs(password+32);
    }
  } else {
      cputs("\n\n\rError!");
  }
  
  gotoxy(22,YPOS2+10);
  cputs(strOKAnyKey);
  resetwndlft();
}

/////////////////////////////////////////////////////////////////////
// The routine to drive the WIFI setting process
//
void DoWifiSetting() {
  static_local bool enter;           //Enter key pressed
  static_local bool success;  
  static_local WifiSettingApple setting;  
  
  #define TEXTCTRLXPOS   2
  #define TEXTCTRLYPOS1  13
  #define TEXTCTRLYPOS2  15
  #define TEXTCTRLWIDTH  36

  setting.version = WIFISETTINGVER;
  setting.authType = WIFIAUTHTYPE;
  
  //Display First Page
  DrawWifiWindowPage1();    

again:
  //Ask user to enter SSID
  enter=EnterText(setting.ssid,SSIDLEN,TEXTCTRLXPOS,TEXTCTRLYPOS1,TEXTCTRLWIDTH);  
  if (!enter) return;
  
  //SSID cannot be empty
  if (setting.ssid[0]=='\0') {
    beep();
    goto again;
  }


  //Display Second Page
  DrawWifiWindowPage2();  

  //Ask user to enter WIFI password
  enter=EnterText(setting.wpakey,WPAKEYLEN,TEXTCTRLXPOS,TEXTCTRLYPOS2,TEXTCTRLWIDTH);    
  if (!enter) return;
  
  //Show window to display the result
  DrawWifiWindowFrame('2',false,false); //Inactivate WIFI window  
  DrawWifiSavedWindowFrame();           //Draw a new window on top
  
  //Save setting to MegaFlash
  success = SaveSetting(CMD_SAVEWIFISETTING,(uint8_t)sizeof(WifiSettingApple),&setting);  
  
  //Show the result
  DrawWifiSavedWindowContent(setting.ssid,setting.wpakey,success);
  
  //Wait the user to acknowledge
  cgetc_showclock();
}

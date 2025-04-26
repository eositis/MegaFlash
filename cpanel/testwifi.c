#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"
#include "ui-menu.h"
#include "ui-wnd.h"
#include "ui-misc.h"
#include "asm.h"
#include "textstrings.h"

// Position and size of Test Wifi window
#define XPOS 4
#define YPOS 3
#define WIDTH 32
#define HEIGHT 18
#define HTAB 24

static const char windowTitle[] = "Test Wifi Connection";


/////////////////////////////////////////////////////////////////////
// Draw Test Wifi Window Frame
//
void DrawTestWifiWindowFrame() {
  wnd_DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,windowTitle,true,true);  
}


void DoTestWifi(){
  uint8_t error;  //Making it static_local results in longer code
  
  //
  //Step 1: Show "Testing" message
  DrawTestWifiWindowFrame();
  cputs("Testing...\n\rIt may take up to 90 seconds.\n\n\rPlease wait.");
  
  //
  //Step 2: Perform the test
  error=TestWifi();
  #ifdef TESTBUILD
  error=ERR_NOERR;
  cgetc_showclock();
  #endif
  DrawTestWifiWindowFrame();
  
  //
  //Step 3: Print the result
  cputs(strResult);
  newline2();
  
  if (error == ERR_UNKNOWN || error == ERR_NOTPICOW) {
    cputs("Unexpected Error.");  
    goto exit;
  }
  
  if (error == ERR_NETTIMEOUT) {
    cputs("Timeout Error.\n\rNo response from MegaFlash.");
    goto exit;
  }
  
  if (error == ERR_SSIDNOTSET) {
    cputs("Wifi has not been setup.\n\n\rPlease select Wifi Setting\n\rfrom Main Menu.");
    goto exit;
  }
  
  if (error == ERR_NONET) {
    cputs("No matching SSID found.");
    goto exit;
  }
  
  if (error == ERR_WIFINOTCONNECTED || error == ERR_BADAUTH) {
    cputs("WIFI not connected.\n\n\rProbably, it is due to\n\rauthentication problem.");   
    goto exit;
  }
  
  if (error == ERR_ABORTED) {
    cputs(strAborted);
    goto exit;
  }
  
  //Wifi Connection OK
  if (error >= ERR_NOIP) {
    cputs("WIFI Connection"); 
    gotox(HTAB);
    cputs(str____OK);
  }

  //DHCP Failed
  if (error == ERR_NOIP) {
    newline2();    
    cputs("DHCP");
    gotox(HTAB);
    cputs(strFailed);
    goto exit;
  }
  
  //Print IP Addresses
  if (error > ERR_NOIP) {
    cputs("\n\r IP Address: ");
    PrintIPAddrFromDataBuffer();
    cputs("\n\r Netmask   : ");
    PrintIPAddrFromDataBuffer();    
    cputs("\n\r Gateway   : ");
    PrintIPAddrFromDataBuffer();    
    cputs("\n\r DNS Server: ");
    PrintIPAddrFromDataBuffer();    
  }

  //DNS Failed/OK
  newline2();  
  cputs("DNS Server");
  gotox(HTAB);  
  if (error == ERR_DNSFAILED) {
    cputs(strFailed);
    goto exit;
  }
  
  if (error > ERR_DNSFAILED) {
    cputs(str____OK);
  }

  //NTP Server Failed/OK
  newline2();  
  cputs("NTP Server");  
  gotox(HTAB);
  if (error == ERR_NTPFAILED) {
    cputs(strFailed);
    goto exit;
  }

  if (error > ERR_NTPFAILED) {
    cputs(str____OK);
  }

  //All tests passed.
  newline2();  
  if (error == ERR_NOERR) {
    cputs("All tests passed.");
  }

exit:
  gotoxy(WIDTH-12,HEIGHT-1);
  cputs(strOKAnyKey);
  cgetc_showclock();
}
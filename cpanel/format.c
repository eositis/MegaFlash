#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>

#include "defines.h"
#include "ui.h"
#include "asm.h"

// Position and size of WIFI setting window
#define XPOS 1
#define YPOS 6
#define WIDTH 38
#define HEIGHT 15

char formatTitle[]    = "Format (1/2)";
char driveNumberPrompt[] = "Drive Number (1- )?";
const char formatPrompt[] = "Format MegaFlash Drive\n\n\r";
const char strVolName[] = " Name  = %s\n\r";
const char strSize[]    = " Size  = %u Blocks\n\r";

extern char strEditPrompt[];
extern const char strOKAnyKey[];

//
// Data to be passed to _DoForamt asm routine
#define VOLNAMELEN 15
char volName[15+1];
uint8_t selectedUnit;
uint16_t blockCount;

//
//Defined in ui.c
extern uint8_t curPos;
extern uint8_t key;
extern char numberInputBuffer[];
extern uint16_t enteredNumber;

// = 0 if ProDOS, !=0 if unknown
// Block Count (Low)
// Block Count (High)
// =0, Reserved
// Volume Name Length
// Volume Name, null terminated.

struct VolInfo {
  uint8_t type;
  uint16_t blockCount;
  uint8_t reserved;
  uint8_t volNameLen;
  char volName[16];
} volInfo;


/////////////////////////////////////////////////////////////////////
// Enter Volume Name
// Only letters, digits and . are allowed.
// The input is stored to volName.
//
bool reenter;   //Set to true if this function is called again to let
                //user to correct the input
bool EnterVolName() {
  static uint8_t x,y;
  
  if (reenter) {
    gotoxy(x,y);  //restore cursor position
  }    
  else {
    curPos=0;
    fillchar('_',VOLNAMELEN);
  } 
  
  //Keyboard event loop
  do {
    key=cgetchar(curPos<VOLNAMELEN?'_':' ');
    if (isalnum(key)|| key=='.'){
      if (curPos<VOLNAMELEN) { 
        cputc(key);
        volName[curPos]=key;
        ++curPos;
      } else beep();
    }
    
    if ((key==KEY_DEL || key==KEY_LEFT) && curPos!=0) {
      --curPos;
      cursorleft();         //Move cursor backward
      cputchar('_');
    }
  
    if (key==KEY_ENTER) {
      volName[curPos]='\0'; //null terminate the string
      x=wherex();           //Save current cursor position
      y=wherey();
      return true;
    }
  }while(key!=KEY_ESC);
 
  //Esc pressed
  volName[0]='\0';   //empty the string
  return false;
}

  


/////////////////////////////////////////////////////////////////////
// Draw Format Window Frame
// 
// Input: page - '1' or '2' for page number display
//
void DrawFormatWindowFrame(char page) {
  formatTitle[8]=page;  
  resetwndlft();  
  DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,formatTitle,true,true);  
  setwndlft(XPOS+1);  
}



/////////////////////////////////////////////////////////////////////
// The routine to drive the formatting process
//
void DoFormat() {
  static_local bool enter;           //Enter key pressed
  static_local uint8_t unitCount;
  static_local char textBuffer[8];  //To store CONFIRM input
  static_local uint8_t error;
  static_local bool success;
  
  static_local uint16_t maxBlockCount;
  
#ifndef TESTBUILD  
  unitCount = GetUnitCount();
  if (unitCount==0) FatalError(ERR_UINTCOUNT_ZERO);
#else
  unitCount = 4;
#endif  
  
  DrawFormatWindowFrame('1');
  
  gotoxy(1,YPOS+HEIGHT-1);
  cputs(strEditPrompt); 
  
  gotoxy(0,YPOS);
  cputs(formatPrompt);

  //
  //Enter Drive Number
  //
  driveNumberPrompt[16]=unitCount+'0';
  cputs(driveNumberPrompt);
  gotox(20);
  curPos = 0;
  enter = EnterNumber(1,1,unitCount);
  if (!enter) goto exit;
  selectedUnit = (uint8_t)enteredNumber;
  
  //
  //Enter Block Count
  //
  newline2();
  
  #ifndef TESTBUILD
  maxBlockCount = GetUnitBlockCount(selectedUnit);
  #else
  maxBlockCount = 65535ul;
  #endif
  
  if (maxBlockCount == 0) FatalError(ERR_GETBLOCKCOUNT_FAIL);
  if (maxBlockCount < 32) FatalError(ERR_GETBLOCKCOUNT_INVALID);
  cprintf("Number of Blocks (32-%u)? ",maxBlockCount);
  
  //Set Default to maxBlockCount
  curPos=snprintf(numberInputBuffer,NUMBERINPUTBUFFERLEN,"%u",maxBlockCount);
  enter = EnterNumber(5,32,maxBlockCount);
  if (!enter) goto exit;
  blockCount = enteredNumber;
  
  newline2();
  cputs("Volume Name:\n\r(A-Z, 0-9 and . only)");
  reenter = false;
  
again:
  gotoxy(13, YPOS+6);
  enter = EnterVolName();  
  if (!enter) goto exit;  
  reenter = true;
  
  gotoxy(0,YPOS+9);
  fillchar(' ',WIDTH-1);
  if (volName[0]=='\0') {
    cputs("Volume name cannot be empty.");
    beep();
    goto again;
  }
  
  if (!isalpha(volName[0])) {
    cputs("Volume name must begin with a letter.");
    beep();
    goto again;
  }
  ToUppercase(volName);
  
//-----------------------------
  
  //Get current volume info
  #ifndef TESTBUILD
  success = GetVolInfo(selectedUnit);
  if (!success) FatalError(ERR_GETVOLINFO_FAIL);
  #else
  volInfo.type = 0;
  volInfo.blockCount = 0xffff;
  strcpy(volInfo.volName, "MEGAFLASH");
  success = success;  //Avoid warning of unused varaibles
  #endif


  DrawFormatWindowFrame('2');

  gotoxy(1,YPOS+HEIGHT-1);
  cputs(strEditPrompt); 
  gotoxy(0,YPOS);
  cputs("Current:\n\r");
  cprintf(" Drive = %u\n\r",selectedUnit);  
  cputs(" Type  = ");
  if (volInfo.type!=0) {
    cputs("Unknown");
    goto printnew;
  }
    
  cputs("ProDOS\n\r");
  cprintf(strVolName,volInfo.volName);
  cprintf(strSize,volInfo.blockCount);
  
printnew:  
  gotoxy(0,12);
  cputs("New:\n\r");
  cprintf(strVolName,volName);
  cprintf(strSize,blockCount);
  newline2();
  cputs("Type CONFIRM to proceed:");

again2:
  enter = EnterText(textBuffer,7,25,YPOS+11,8);
  if (enter==false) goto exit;
    
  ToUppercase(textBuffer);
  if (strcmp(textBuffer,"CONFIRM")!=0) {
    beep();
    goto again2;
  }
    
//-----------------------------
  DrawFormatWindowFrame('2');
  
  gotoxy(0,YPOS);
  cputs("Formatting...\n\n\r");
  #ifndef TESTBUILD
  error = FormatDisk(); //Execute Format
  #else
  error = 0;
  #endif
  
  if (error==0) cputs("Formatting completed.");
  else cprintf("Error:$%x",error);
  gotoxy(26,YPOS+14);
  cputs(strOKAnyKey);
  
  cgetc();

exit:
  resetwndlft();
}



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


// Position and size of WIFI setting window
#define XPOS 1
#define YPOS 6
#define WIDTH 38
#define HEIGHT 15

static char formatWindowTitle[]  = "Format (1/3)";
static char driveNumberPrompt[]  = "Drive Number (1- )? ";
static const char volNameFormatStr[]   = " Name  = %s\n\r";
static const char sizeFormatStr[]      = " Size  = %u Blocks\n\r";

//
// Variable to pass data to _DoFormat asm routine
char     fmt_volName[VOLNAMELEN+1];
uint8_t  fmt_selectedUnit;
uint16_t fmt_blockCount;


/////////////////////////////////////////////////////////////////////
// Draw Format Window Frame
// 
// Input: page - '1' , '2' or '3' for page number display
//
void DrawFormatWindowFrame(char page) {
  formatWindowTitle[8]=page;  
  wnd_DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,formatWindowTitle,true,true);  
}



/////////////////////////////////////////////////////////////////////
// The routine to drive the formatting process
//
void DoFormat() {
  static_local bool enter;           //Enter key pressed
  static_local uint8_t unitCount;
  static_local uint8_t error;
  static_local uint16_t maxBlockCount;
  static_local VolInfo_t volInfo; //Data structure returned by CMD_GETVOLINFO command
  
  ///////////////////////////////////////////////////////////
  //
  //    Page 1
  //
  ///////////////////////////////////////////////////////////  
  
  unitCount = GetUnitCount();
  if (unitCount==0) FatalError(ERR_UINTCOUNT_ZERO);
  
  DrawFormatWindowFrame('1');
  gotoxy(1,HEIGHT-1);
  cputs(strEditPrompt);
  
  gotoxy00();
  PrintDriveInfoList(unitCount);
  newline();
   
  //
  //Enter Drive Number
  //
  driveNumberPrompt[16]=unitCount+'0';
  cputs(driveNumberPrompt);
  enter=ti_EnterNumber(1,1,unitCount);
  if (!enter) return;
  fmt_selectedUnit = (uint8_t) ti_enteredNumber;
  
  ///////////////////////////////////////////////////////////
  //
  //    Page 2
  //
  ///////////////////////////////////////////////////////////
  
  //
  //Enter Block Count
  //
  DrawFormatWindowFrame('2');
  gotoxy(1,HEIGHT-1);
  cputs(strEditPrompt);   
  
  maxBlockCount = GetUnitBlockCount(fmt_selectedUnit);
  if (maxBlockCount == 0) FatalError(ERR_GETBLOCKCOUNT_FAIL);
  if (maxBlockCount < 32) FatalError(ERR_GETBLOCKCOUNT_INVALID);
  gotoxy00();
  cprintf("Number of Blocks (32-%u)? ",maxBlockCount);
  
  enter = ti_EnterNumberDefault(5,32,maxBlockCount,maxBlockCount); //Set Default to maxBlockCount
  if (!enter) return;
  fmt_blockCount = ti_enteredNumber;
  
  //
  // Enter Volume Name
  //
  newline2();
  cputs("Volume Name:\n\r(A-Z, 0-9 and . only)");
  
  ti_textBuffer[0]='\0';
again:
  enter = ti_EnterVolName(13,2);  
  if (!enter) return;

  gotoxy(0,5);
  clreol();
  if (ti_textBuffer[0]=='\0') {
    beep();
    goto again;
  }
  
  if (!isalpha(ti_textBuffer[0])) {
    cputs("Volume name must begin with a letter.");
    beep();
    goto again;
  }
  
  //Volume Name is ok
  strcpy(fmt_volName,ti_textBuffer);
  ToUppercase(fmt_volName);
  
  ///////////////////////////////////////////////////////////
  //
  //    Page 3
  //
  ///////////////////////////////////////////////////////////
  
  //Get current volume info
  if (!GetVolInfo(fmt_selectedUnit,&volInfo)) FatalError(ERR_GETVOLINFO_FAIL);

  DrawFormatWindowFrame('3');
  gotoxy(1,HEIGHT-1);
  cputs(strEditPrompt); 
  
  //Print current volume info
  gotoxy00();
  cputs(strCurrent);
  newline();
  cprintf(" Drive = %u",fmt_selectedUnit);  
  newline();
  cputs(" Type  = ");
  PrintVolumeType(volInfo.type);
  newline();
  if (volInfo.type==TYPE_PRODOS) {
    cprintf(volNameFormatStr,volInfo.volName);
    cprintf(sizeFormatStr,volInfo.blockCount);
  }
  
  //Print new volume info
  gotoxy(0,6);
  cputs(strNew);
  newline();
  cprintf(volNameFormatStr,fmt_volName);
  cprintf(sizeFormatStr,fmt_blockCount);
  newline2();

  //Ask user to type CONFIRM
  enter = AskUserToConfirm();
  if (enter==false) return;
    
  ///////////////////////////////////////////////////////////
  //
  //    Page 3 (Result)
  //
  ///////////////////////////////////////////////////////////
  DrawFormatWindowFrame('3');
  
  cputs(strFormatting);
  newline2();
  error = FormatDisk(); //Execute Format
  
  if (error==0) cputs("Disk Format completed.");
  else cprintf("Error:$%x",error);
  gotoxy(26,HEIGHT-1);
  cputs(strOKAnyKey);
  
  cgetc_showclock();
}



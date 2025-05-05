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


// Position and size of Format window
#define XPOS 1
#define YPOS 6
#define WIDTH 38
#define HEIGHT 15

static char formatWindowTitle[]  = "Format (1/3)";


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
static void DrawFormatWindowFrame(char page) {
  formatWindowTitle[8]=page;  
  wnd_DrawWindow(XPOS,YPOS,WIDTH,HEIGHT,formatWindowTitle,true,true);  
}




/////////////////////////////////////////////////////////////////////
// The routine to drive the formatting process
//
void DoFormat() {
  static_local uint8_t unitCount;
  static_local uint8_t error;
  static_local uint16_t maxBlockCount;
  
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
  strDriveNumberPrompt[16]=unitCount+'0';
  cputs(strDriveNumberPrompt);
  if (!ti_EnterNumber(1,1,unitCount)) return;
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
  
  //Set Default to maxBlockCount
  if (!ti_EnterNumberDefault(5,32,maxBlockCount,maxBlockCount)) return;
  fmt_blockCount = ti_enteredNumber;
  
  //
  // Enter Volume Name
  //
  newline2();
  cputs("Volume Name:\n\r(A-Z, 0-9 and . only)");
  
  ti_textBuffer[0]='\0';
again:
  if (!ti_EnterVolName(13,2)) return;

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
  DrawFormatWindowFrame('3');
  gotoxy(1,HEIGHT-1);
  cputs(strEditPrompt); 
  
  //Print current volume info
  gotoxy00();
  PrintDriveInfo(fmt_selectedUnit);
    
  //Print new volume info
  gotoxy(0,6);
  cputs(strNew);
  newline();
  cprintf(strVolNameFormat,fmt_volName);
  cprintf(strVolSizeFormat,fmt_blockCount);
  newline2();

  //Ask user to type CONFIRM
  if (!AskUserToConfirm()) return;
    
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



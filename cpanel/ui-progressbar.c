#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "defines.h"
#include "asm.h"
#include "ui-progressbar.h"

//Variable to store the position and size of progressbar
static uint8_t x,y,width;
static uint8_t lastValue;
static bool shown=false;

////////////////////////////////////////////////////////////////
// Show Progress Bar
// 
// Input: x,y  - position of the progress bar
//        width - width of the progress bar
//
//two addtional column is needed to draw the progressbar
//So, width <= Window Width-2
void ShowProgressBar(uint8_t _x,uint8_t _y,uint8_t _width) {
  width = _width;
  y = _y;
  x = _x;
  lastValue = 0;
  shown = true;
  
  gotoxy(x,y);
  //We want the progress bar to be as wide as possible.
  //So, we need to draw the right bar at xpos=-1. i.e.outside
  //current scroll window. We can cheat by reducing BASL ($28) by 1
  __asm__("dec $28");   //reduce BASL by 1
  cputchar_direct(RIGHT_BAR);
  __asm__("inc $28");   //restore BASL
  fillchar_direct(DUAL_BAR,width);
  gotox(x+width);
  cputc(LEFT_BAR);
}

/////////////////////////////////////////////////////////////
// Hide the pevious shown progress bar
//
void HideProgressBar() {
  if (!shown) return;
  
  shown = false;
  gotoxy(x,y);
  __asm__("dec $28");
  cputchar_direct(' ');
  __asm__("inc $28");
  clreol();
}

//////////////////////////////////////////////////////////////
// Update the progress bar disply
//
// Input: value (0-width)
//
void UpdateProgressBar(uint8_t value) {
  if (!shown) return; 
  if (value!=lastValue) {
    lastValue = value;
    gotoxy(x,y);
    fillchar_direct(WHITE_BLOCK,value>=width?width:value);
  }
}
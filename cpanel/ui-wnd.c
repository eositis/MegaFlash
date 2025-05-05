#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <peekpoke.h>
#include "defines.h"
#include "ui-wnd.h"
#include "asm.h"

//Working variables
static uint8_t i;

/////////////////////////////////////////////////////////////////////
// Reset Scroll Window Position
//
void wnd_ResetScrollWindow() {
  POKE(WNDLFT,WNDLFT_DEFAULT);
  POKE(WNDWDTH,WNDWDTH_DEFAULT);
  POKE(WNDTOP,WNDTOP_DEFAULT);
  POKE(WNDBTM,WNDBTM_DEFAULT);
  gotoxy00(); //To make changes effective
}



/////////////////////////////////////////////////////////////////////
// Draw a box on screen
// 
// Input: x,y - location of content area
//        width,height - size of content area
//
void wnd_DrawBox(uint8_t x,uint8_t y,uint8_t width,uint8_t height) {
  static_local uint8_t x2,y2;
  
  x2=x+width;
  y2=y+height;
  
  gotoxy(x,y-1);
  fillchar_direct(BOTTOM_BAR,width);
  
  for(i=y;i<y2;++i) {
    gotoxy(x-1,i);
    cputchar_direct(RIGHT_BAR);
    gotox(x2);
    cputchar_direct(LEFT_BAR);
  }
  
  gotoxy(x,y2);
  fillchar_direct(TOP_BAR,width);
}


/////////////////////////////////////////////////////////////////////
// Draw a window on screen and set scroll window
// 
// Input: x,y - location of content area
//        width,height - size of content area
//        title - title string
//        isActive - window is active or inactive
//        clearContentArea - To clear the content area
//
// The top row is occupied by the topbar characters. The leftmost
// column is not used by us. So, we set the scroll window like this:
//  _______________
// |_______________|
// |               |
// | xxxxxxxxxxxxxx|  
// | xxxxxxxxxxxxxx|
// | xxxxxxxxxxxxxx|
//  ¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯
//
void wnd_DrawWindow(uint8_t x,uint8_t y,uint8_t width,uint8_t height,const char* title, bool isActive,bool clearContentArea) {
  wnd_ResetScrollWindow();  //Reset screen coordinates
    
  //Draw Title Bar
  gotoxy(x,y-2);
  fillchar_direct(isActive?WHITE_BLOCK:' ',width);
  cputc(' ');
  cputs(title);
  cputc(' ');  
  
  //To optimize the performance, don't draw the frame if isActive is false.
  //The only purpose of setting isActive to false is to deactivate current window
  //In this case, we just need to redraw the title bar.
  if (isActive) {
    //Extend the box 2 row upwards for title bar
    wnd_DrawBox(x,y-2,width,height+2);  
    
    //Horizontal Bar under Title
    gotoxy(x,y-1);
    fillchar_direct(TOP_BAR,width);
    
    //Clear Content Area if clearContentArea is true
    if (clearContentArea) {
      for(i=y+height-1;i>=y;--i) {  
        gotoxy(x, i);
        fillchar_direct(' ',width);
      }
    }
  }
  
  //Setup scroll window
  POKE(WNDTOP,y);
  POKE(WNDLFT,x+1);
  POKE(WNDWDTH,width-1);
  POKE(WNDBTM,y+height);
  gotoxy00(); //To make scroll window effective
}




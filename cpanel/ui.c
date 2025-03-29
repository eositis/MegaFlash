#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "defines.h"
#include "asm.h"
#include "ui.h"

//From main.c
void ShowClock();

//Global Variables
//Note: Global Variables is faster than stack variables
static uint8_t i,x2,y2;
uint8_t key;
uint8_t curPos;

/////////////////////////////////////////////////////////////////////
// Draw a box on screen
// 
// Input: x,y - location of content area
//        width,height - size of content area
//
void DrawBox(uint8_t x,uint8_t y,uint8_t width,uint8_t height) {
  x2=x+width;
  y2=y+height;
  
  gotoxy(x,y-1);
  fillchar(BOTTOM_BAR,width);
  
  for(i=y;i<y2;++i) {
    gotoxy(x-1,i);
    cputchar(RIGHT_BAR);
    gotox(x2);
    cputchar(LEFT_BAR);
  }
  
  gotoxy(x,y2);
  fillchar(TOP_BAR,width);
}

#if 0
//Demo of DrawBox()
void DrawBoxDemo() {
  uint8_t x=4;
  uint8_t y=5;
  
  //Draw Content
  //width=5, height =2
  gotoxy(x,y);
  cputs("ABCDE");
  gotoxy(x,y+1);
  cputs("12345");
  
  DrawBox(4,5,5,2);
}
#endif

/////////////////////////////////////////////////////////////////////
// Draw a window on screen
// 
// Input: x,y - location of content area
//        width,height - size of content area
//        title - title string
//        isActive - window is active or inactive
//        clearContentArea - To clear the content area
//
void DrawWindow(uint8_t x,uint8_t y,uint8_t width,uint8_t height,const char* title, bool isActive,bool clearContentArea) {
  y2 = y-2;  
  
  //Draw Title Bar
  gotoxy(x,y2);
  fillchar(isActive?WHITE_BLOCK:' ',width);
  gotoxy(x,y2);
  cputc(' ');
  cputs(title);
  cputc(' ');  
  
  //Extend the box 2 row upwards for title bar
  DrawBox(x,y2,width,height+2);  
  
  //Horizontal Bar under Title
  gotoxy(x,y-1);
  fillchar(TOP_BAR,width);
  
  //Clear Content Area if clearContentArea is true
  if (clearContentArea) {
    for(i=y+height-1;i>=y;--i) {  
      gotoxy(x, i);
      fillchar(' ',width);
    }
  }
}

#if 0
//Demo of DrawWindow()
void DrawWindowDemo() {
  uint8_t x=30;
  uint8_t y=15;
  
  //Draw Content
  //width=7, height =2
  gotoxy(x,y);
  cputs("ABCDEF");
  gotoxy(x,y+1);
  cputs("123456");
  
  DrawWindow(x,y,6,2,"Test",0,false);
  
  DrawWindow(1,12,25,10,"Active",0,false);
}
#endif

/////////////////////////////////////////////////////////////////////
// Replace cgetc()
// Call ShowClock() regularly while waiting for a key
//
// output: char - key pressed
//
char cgetc_showclock() {
  static_local uint16_t count;  
  
  do {
    if (count==0) ShowClock();    
    ++count;
  }while(!kbhit());

  return cgetc();
}


/////////////////////////////////////////////////////////////////////
// Display and handle menu selection
// 
// Input: menuItem    - array of menu item strings
//        itemCount   - Number of menu items
//        menuX,menuY - Position of the menu
//
// Output: uint8_t    - key pressed
//
// To optimize the code, currentMenuItem is a global variable.
// It stores the default item when this function is called.
// It also stores the item selected by user when this function returns.
//
// The function returns only when user presses Enter, Esc,
// Left or Right key
//
uint8_t currentMenuItem;
uint8_t DoMenu(const char* menuItems[],
               uint8_t itemCount,
               uint8_t menuX,
               uint8_t menuY) {
  static_local uint8_t itemCountMinus1; //caching itemCount-1 reduces code size
  itemCountMinus1 = itemCount-1;               
                 
  do {  
    gotoxy(menuX,menuY);  
    for(i=0;i<itemCount;++i) {
      if (i==currentMenuItem) revers(true);
      cputs(menuItems[i]);
      revers(false);
      newlinex(menuX);
    }
    
    key = cgetc_showclock();
    if (key==KEY_UP) {
      if (currentMenuItem!=0) --currentMenuItem;
      else currentMenuItem = itemCountMinus1; //wrap to the last item
    }
    else if (key==KEY_DOWN) {
      if (currentMenuItem!=itemCountMinus1) ++currentMenuItem;
      else currentMenuItem = 0;  //wrap to the first item
    }
  } while (key!=KEY_ENTER && key!=KEY_ESC && key!=KEY_RIGHT && key!=KEY_LEFT);

 return key ;
}


/////////////////////////////////////////////////////////////////////
// Text Input
// 
// Input: buffer - string buffer to store the inputted text
//                 size >= len+1 (To store null character)
//        len    - maximum lenghth of the input
//        x,y    - position of the text input control
//        width  - maximum width of text input control
//
// Output: true  - user has hitted Enter
//         false - user has hitted Esc
//
// The function returns only when user hits Enter or Esc key
bool EnterText(char* buffer,uint8_t len,uint8_t x,uint8_t y,uint8_t width) {
  
  curPos=0;
  x2= x+width;
  
  //Draw the text input lines
  i=len;
  gotoxy(x,y);
  do{
    if (i<=width) {
      fillchar('_',i);
      break;
    } else {
      fillchar('_',width);
      i = i - width;
      cursordown();
    }
  }while (i!=0);
  
  //Keyboard event loop
  gotoxy(x,y);
  do {
    key=cgetchar(curPos<len?'_':' ');
    if (key>=32 && key<=126) {
      if (curPos<len) { 
        cputc(key);
        if (wherex()==x2) newlinex(x);  //next line
        buffer[curPos]=key;
        ++curPos;
      } else beep();
    }
    if ((key==KEY_DEL || key==KEY_LEFT) && curPos!=0) {
      --curPos;
      buffer[curPos]='\0';  //Remove the character from buffer
      cursorleft();         //Move cursor backward
      if (wherex()<x && curPos!=0) cursorupx(x2-1); //Move to the end of previous line
      cputchar('_');
    }
  
    if (key==KEY_ENTER) {
      buffer[curPos]='\0'; //null terminate the string
      return true;
    }
  }while(key!=KEY_ESC);
 
  //Esc pressed
  buffer[0]='\0';   //empty the string
  return false;
}




/////////////////////////////////////////////////////////////////////
// Number Input
// The number is returned by global variable enteredNumber.
// 
// Input: len    - maximum lenghth of the input
//        min    - minimum value of valid input
//        max    - maximum value of valid input
//
// Output: true  - user has hitted Enter
//         false - user has hitted Esc
//
// The function returns only when user hits Enter or Esc key.
// If the number entered is out of valid range, it beeps and
// does not accept Enter key.
//
// To set a default value, initalize numberInputBuffer
// and set curPos to the length of the string
// If no default value, set curPos to 0
//
uint16_t enteredNumber;             //return value
char numberInputBuffer[NUMBERINPUTBUFFERLEN];
bool EnterNumber(uint8_t len,uint16_t min,uint16_t max) {
  
  fillchar('_',len);
  if (curPos!=0) cputs(numberInputBuffer);
  
  do {
    key=cgetchar(curPos<len?'_':' ');
    if (key>='0' && key<='9') {
      if (curPos<len) { 
        cputc(key);
        numberInputBuffer[curPos]=key;
        ++curPos;      
      } else beep(); 
    }
    
    if ((key==KEY_DEL || key==KEY_LEFT) && curPos!=0) {
      --curPos;
      cursorleft();         //Move cursor backward
      cputchar('_');
    }
    
    if (key==KEY_ENTER) {
      enteredNumber = 0;
      
      for(i=0;i<curPos;++i) {
        enteredNumber *= 10;
        enteredNumber += numberInputBuffer[i]-'0';
      }
      
      //Check if the number overflow the range of 16-bit integer
      //i.e. Number of Digit = 5 && numberInputBuffer[0]>='6' but enteredNumber<60000
      //For example, if "70000" is entered, enteredNumber becomes 4464 due overflow of uint16_t     
      //Use 0xe000 (57344) instead of 60000 to reduce code size
      if (curPos==5 && numberInputBuffer[0]>='6' && enteredNumber<0xe000u) {
        beep();
        continue;
      }
      
      if (enteredNumber<min || enteredNumber >max) beep();
      else {
        fillchar(' ',len-curPos); //Remove the underscore chars
        return true;
      }
    }
  } while(key!=KEY_ESC);
  
  return false;
}  


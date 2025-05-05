#define _UI_TEXTINPUT_C
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "defines.h"
#include "asm.h"
#include "ui-textinput.h"

// All exported variables and functions are prefix with ti_ (textinput)
// Inputted String is stored in ti_textBuffer.
// For ti_EnterNumber(), the number inputted is stored in ti_enteredNumber
// variable

// To store the result of ti_EnterNumber()
uint16_t ti_enteredNumber;


// Variables to pass parameter to EnterTextImpl
static bool disallowSpace;
static bool numbersOnly;
static bool volnameOnly;  //Only allow alphanumeric character and period (.)
static uint8_t x,y,len,x2;
static uint8_t width;     //Width of the text input control for multiline edit. Set to -1 for singleline

// Working Variables
static uint8_t i,curPos;
static bool enter;


static void PrintTextEditCtrl(char underscore_char) {
  static_local uint8_t curx,cury;
  static_local char c;
  //
  //Print underscore and default text
  //
  curPos = 0xff;    //To indicate end of default text has not been found.
  gotoxy(x,y);
  for (i=0;i<=len;++i) {
    c = ti_textBuffer[i];
    if (c=='\0' && curPos==0xff) {
      curPos = i;   //Mark the end of default text
      curx = wherex();  //Save the current position of cursor
      cury = wherey();
    }
    
    //print underscore or default text character
    if (i!=len) cputc(curPos==0xff?c:underscore_char);
    
    //move to next line unless reaching the last char.
    if (wherex()==x2 && i!=len-1) newlinex(x);
  }
  //Place the cursor at the end of default text
  gotoxy(curx,cury);
}

///////////////////////////////////////////////////////////////////////
// There are different variants of input routines. e.g. ti_EnterText(),
// ti_EnterHostname() and ti_EnterNumber(). The underlying implementation
// is this function EnterTextImpl. 
// Front-end functions like ti_EnterText() is exported. The parameters are
// passed to EnterTextImpl() through static global variables.
//
static bool EnterTextImpl() {

  static_local uint8_t key;
  x2= x+width;  //Use x2 to cache this value

  PrintTextEditCtrl('_');

  
  //
  //Keyboard event loop
  //
  do {
    key=cgetchar(curPos<len?'_':' ');
    if (disallowSpace && key==' ') {
      beep();
      continue;
    }
    
    if (key>=32 && key<=126) {
      if (numbersOnly && (key<'0'||key>=('9'+1))) {
        beep();
        continue;
      }
      
      if (volnameOnly && (!isalnum(key) && key!='.')) {
        beep();
        continue;
      }
      
      if (curPos<len) { 
        ti_textBuffer[curPos]=key;
        ++curPos;
        cputc(key);
        if (wherex()==x2 && curPos!=len) newlinex(x);  //next line. Dont move to next line if reach the last char.
      } else beep();
    }
    
    else if (key==KEY_DEL || key==KEY_LEFT) {
      if (curPos!=0) {
        --curPos;
        ti_textBuffer[curPos]='\0';       //Remove the character from ti_textBuffer
        if (wherex()==x) cursorupx_m1(x2);  //Move to the end of previous line (xpos=x2-1)
        else cursorleft();                //Move cursor backward
        cputchar_direct('_');
      } else beep();
    }
  
    else if (key==KEY_ENTER) {
      ti_textBuffer[curPos]='\0'; //null terminate the string
      return true;
    }
  }while(key!=KEY_ESC);
 
  //Esc pressed
  ti_textBuffer[0]='\0';   //empty the string
  return false;
}


/////////////////////////////////////////////////////////////////////
// Text Input
// 
// Input: 
//        len           - maximum length of the input (Max: 255)
//        x,y           - position of the text input control
//        width         - maximum width of text input control
//
// Output: true  - user has hitted Enter
//         false - user has hitted Esc
//
// ti_textBuffer is 256 bytes long. One byte is needed to store the NULL
// char. So, the maximum length of the text is 255.
//
// To set default input, copy the text to ti_textBuffer.
// If there is no default input, set ti_textBuffer[0] to '\0'
//
// The function returns only when user hits Enter or Esc key
//
bool ti_EnterText(uint8_t _len,uint8_t _x,uint8_t _y,uint8_t _width){
  width = _width;
  y = _y;
  x = _x;
  len=_len;
  disallowSpace = false;
  numbersOnly = false;
  volnameOnly = false;

  return EnterTextImpl();
}

/////////////////////////////////////////////////////////////////////
// ProDOS Volume Name Input
// Only alphanumeric and period (.) characters are allowed.
// Length is fixed to 15 characters.
// 
// Input: 
//        x,y           - position of the text input control
//
// Output: true  - user has hitted Enter
//         false - user has hitted Esc
//
// To set default input, copy the text to ti_textBuffer.
// If there is no default input, set ti_textBuffer[0] to '\0'
//
// The function returns only when user hits Enter or Esc key
//
bool ti_EnterVolName(uint8_t _x,uint8_t _y) {
  y = _y;
  x = _x;
  len = VOLNAMELEN;
  width = -1;   //Single Line only;
  disallowSpace = false;
  numbersOnly = false;
  volnameOnly = true;
  
  return EnterTextImpl();
}

/////////////////////////////////////////////////////////////////////
// Hostname Input
// Space characters are nott allowed.
// 
// Input: 
//        len           - maximum length of the input (Max: 255)
//        x,y           - position of the text input control
//        width         - maximum width of text input control
//
// Output: true  - user has hitted Enter
//         false - user has hitted Esc
//
// ti_textBuffer is 256 bytes long. One byte is needed to store the NULL
// char. So, the maximum length of the text is 255.
//
// To set default input, copy the text to ti_textBuffer.
// If there is no default input, set ti_textBuffer[0] to '\0'
//
// The function returns only when user hits Enter or Esc key
//
bool ti_EnterHostname(uint8_t _len,uint8_t _x,uint8_t _y,uint8_t _width){
  width = _width;
  y = _y;
  x = _x;
  len=_len;
  disallowSpace = true;
  numbersOnly = false;
  volnameOnly = false;

  return EnterTextImpl();
}

/////////////////////////////////////////////////////////////////////
// Common Implementation of ti_EnterNumber() and ti_EnterNumberDefault()
//
static bool EnterNumberImpl(uint16_t min,uint16_t max){
  width = -1;
  disallowSpace = false;
  numbersOnly = true;
  volnameOnly = false;
  
  while (1) {
    enter = EnterTextImpl();
    if (!enter) return false;   //Esc Key pressed
    
    //Convert ASCII to number
    ti_enteredNumber = 0;
    for(i=0;i<curPos;++i) {
      ti_enteredNumber *= 10;
      ti_enteredNumber += ti_textBuffer[i]-'0';
    }  
    
    //Check if the number overflow the range of 16-bit integer
    //i.e. Number of Digit = 5 && ti_textBuffer[0]>='6' but ti_enteredNumber<60000
    //For example, if "70000" is entered, ti_enteredNumber becomes 4464 due overflow of uint16_t     
    //Use 0xe000 (57344) instead of 60000 to reduce code size
    if (curPos==5 && ti_textBuffer[0]>='6' && ti_enteredNumber<0xe000u) {
      beep();
      continue;
    }  
    
    //Out of Range?
    if (ti_enteredNumber<min || ti_enteredNumber >max) {
      beep();
      continue;
    }
    else {
      fillchar_direct(' ',len-curPos); //Everything is ok. Remove the underscore chars
      return true;
    }
  }
}

/////////////////////////////////////////////////////////////////////
// Number Input without default value at current cursor position
// 
// Input: 
//        len           - maximum length of the input (Max: 255)
//        min,max       - Valid range of the number
//
// Output: true  - user has hitted Enter
//         false - user has hitted Esc
//
// Inputted number is stored in ti_enteredNumber variable
//
bool ti_EnterNumber(uint8_t _len,uint16_t min,uint16_t max){
  ti_textBuffer[0]='\0';
  len = _len;
  x   = wherex();
  y   = wherey();
  return EnterNumberImpl(min,max);
}

/////////////////////////////////////////////////////////////////////
// Number Input with default value at current cursor position
// 
// Input: 
//        len           - maximum length of the input (Max: 255)
//        min,max       - Valid range of the number
//        defaultValue  - Default Value
//
// Output: true  - user has hitted Enter
//         false - user has hitted Esc
//
// Inputted number is stored in ti_enteredNumber variable
//
bool ti_EnterNumberDefault(uint8_t _len,uint16_t min,uint16_t max,uint16_t defaultValue){
  sprintf(ti_textBuffer,"%u",defaultValue);
  len = _len;
  x   = wherex();
  y   = wherey();
  return EnterNumberImpl(min,max);
}



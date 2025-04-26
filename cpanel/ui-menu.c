#define _UI_MENU_C
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "defines.h"
#include "asm.h"
#include "ui-menu.h"
#include "ui-misc.h"

//Global Variable
uint8_t mnu_currentMenuItem;

/////////////////////////////////////////////////////////////////////
// Display and handle menu selection
// 
// Input: menuItem    - array of menu item strings
//        itemCount   - Number of menu items
//        menuX,menuY - Position of the menu
//
// Output: uint8_t    - key pressed
//
// To optimize the code, mnu_currentMenuItem is a global variable.
// It stores the default item when this function is called.
// It also stores the item selected by user when this function returns.
//
// The function returns only when user presses Enter, Esc,
// Left or Right key
//
uint8_t DoMenu(const char* menuItems[],uint8_t itemCount,uint8_t menuX,uint8_t menuY) {

  static_local uint8_t i,key;                 
  static_local uint8_t itemCountMinus1; //caching itemCount-1 reduces code size
  itemCountMinus1 = itemCount-1;               

  do {  
    gotoxy(menuX,menuY);  
    for(i=0;i<itemCount;++i) {
      if (i==mnu_currentMenuItem) revers(true);
      cputs(menuItems[i]);
      revers(false);
      newlinex(menuX);
    }
    
    key = cgetc_showclock();
    if (key==KEY_UP) {
      if (mnu_currentMenuItem!=0) --mnu_currentMenuItem;
      else mnu_currentMenuItem = itemCountMinus1; //wrap to the last item
    }
    else if (key==KEY_DOWN) {
      if (mnu_currentMenuItem!=itemCountMinus1) ++mnu_currentMenuItem;
      else mnu_currentMenuItem = 0;  //wrap to the first item
    }
  } while (key!=KEY_ENTER && key!=KEY_ESC && key!=KEY_RIGHT && key!=KEY_LEFT);

 return key ;
}







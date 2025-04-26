#ifndef _UI_MENU_H
#define _UI_MENU_H
#include <stdint.h>
#include <stdbool.h>

#ifndef _UI_MENU_C
extern uint8_t mnu_currentMenuItem;
#endif

uint8_t DoMenu(const char* menuItems[],uint8_t itemCount,uint8_t menuX,uint8_t menuY);




#endif

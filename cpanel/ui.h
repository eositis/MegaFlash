#ifndef _UI_H
#define _UI_H
#include <stdint.h>
#include <stdbool.h>

void DrawBox(uint8_t x,uint8_t y,uint8_t width,uint8_t height);
void DrawBoxDemo();
void DrawWindow(uint8_t x,uint8_t y,uint8_t width,uint8_t height,const char* title, bool isActive,bool clearContentArea);
void DrawWindowDemo();
uint8_t DoMenu(const char* menuItems[],uint8_t itemCount,uint8_t menuX,uint8_t menuY);
bool EnterText(char* buffer,uint8_t len,uint8_t x,uint8_t y,uint8_t width);
char cgetc_showclock();

bool EnterNumber(uint8_t len,uint16_t min,uint16_t max);
bool EnterVolName();
//bool IsLetter(char c);

#define NUMBERINPUTBUFFERLEN 6  /* 5 Digits + Null */

#endif

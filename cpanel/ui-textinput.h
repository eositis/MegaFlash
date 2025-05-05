#ifndef _UI_TEXTINPUT_H
#define _UI_TEXTINPUT_H
#include <stdint.h>
#include <stdbool.h>

#ifndef _UI_TEXTINPUT_C
extern uint16_t ti_enteredNumber;
#endif

//Use $200-$2FF as input text buffer
#define ti_textBuffer     ((char*)0x200)

bool ti_EnterText(uint8_t _len,uint8_t _x,uint8_t _y,uint8_t _width);
bool ti_EnterHostname(uint8_t _len,uint8_t _x,uint8_t _y,uint8_t _width);
bool ti_EnterVolName(uint8_t _x,uint8_t _y);
bool ti_EnterNumber(uint8_t _len,uint16_t min,uint16_t max);
bool ti_EnterNumberDefault(uint8_t _len,uint16_t min,uint16_t max,uint16_t defaultValue);
void ti_ClearUnderscore();

#endif
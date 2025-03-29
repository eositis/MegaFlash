#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <stdint.h>
#include <stdbool.h>

#include "defines.h"
#include "asm.h"

extern ConfigApple config;


//TZHOUR and TZMIN is defined in ../common/defines.h and shared with Pico Project
const int8_t tzhour[]= TZHOUR;
const uint8_t tzmin[]= TZMIN;

#define TIMEZONECOUNT (sizeof(tzhour)/sizeof(tzhour[0]))


/////////////////////////////////////////////////////////////////////
// Toggle Timezone Setting
// Assume cursor position has been set.
// 
// Input: key - user input
// If key is not Left or Right keys, the timezone display is updated.
//
void DoToggleTimezone(uint8_t key){
  static_local uint8_t timezoneid; 
  static_local int8_t hour;
  
  timezoneid = config.timezoneid;
  
  if (key==KEY_ENTER) return;
  else if (key==KEY_LEFT  && timezoneid!=0) --timezoneid;
  else if (key==KEY_RIGHT && timezoneid!=TIMEZONECOUNT-1) ++timezoneid;
  
  hour = tzhour[timezoneid];
  if (hour>=-9 && hour<=9) cputc(' ');
  
  cprintf("UTC%+2d:%02u",hour,tzmin[timezoneid]);
  
  config.timezoneid = timezoneid;
}
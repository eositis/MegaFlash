#ifndef _DEFINES_H
#define _DEFINES_H

#include "../common/defines.h"

//For CC65, global or static variables are usually faster than
//stack variables. So, some local variables are declared as
//static_local, which is defined as static to reduce code size.
#define static_local static

//
//MegaFlash Registers
#define CMDREG    0xc0c0  //Command Register
#define STATUSREG 0xc0c1  //Status Register
#define PARAMREG  0xc0c2  //Parameter Register
#define IDREG     0xc0c3  //ID Register


//
//Key Codes
#define KEY_UP    11
#define KEY_DOWN  10
#define KEY_LEFT  8
#define KEY_RIGHT 21
#define KEY_DEL   127
#define KEY_ESC   27
#define KEY_ENTER 13

//Mousetext Char
#define LEFT_BAR 223
#define RIGHT_BAR 218
#define TOP_BAR 204     //'\314'
#define BOTTOM_BAR '_'
#define WHITE_BLOCK 160
#define TICK     196    //'\304'
#define CHECKER1 214
#define CHECKER2 215
#define ENTER    205    //'\315'
#define UPARROW  203    //'\313'
#define DNARROW  202    //'\312'
#define LTARROW  200    //'\310'
#define RTARROW  213    //'\325'
#define CDASH    211    //'\323'


enum ERROR {
  ERR_CONFIG_INVALID,
  ERR_SAVECONFIG_FAIL,
  ERR_UINTCOUNT_ZERO,
  ERR_GETVOLINFO_FAIL,
  ERR_GETBLOCKCOUNT_FAIL,
  ERR_GETBLOCKCOUNT_INVALID
};

//Function Prototypes
void FatalError(uint8_t errorcode);

#endif
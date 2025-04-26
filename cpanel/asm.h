#ifndef _ASM_H
#define _ASM_H

#include <stdint.h>


//
// asm-megaflash.s
//
void __fastcall__ SendCommand();
uint8_t __fastcall__ GetBoardType();
uint8_t __fastcall__ DisableROMDisk();
uint8_t __fastcall__ GetUnitCount();
uint8_t __fastcall__ FormatDisk();
bool __fastcall__ GetVolInfo(uint8_t unitNum,void* pVolInfo);
uint8_t __fastcall__ TestWifi();
void __fastcall__ EraseAllConfig(); 
uint16_t __fastcall__ GetUnitBlockCount(uint8_t unitNum);
void __fastcall__ DisplayTime();
bool __fastcall__ SaveSetting(uint8_t cmd, uint8_t len, void* src);
void __fastcall__ LoadSetting(uint8_t cmd, uint8_t len, void* dest);
void __fastcall__ PrintIPAddrFromDataBuffer();
void __fastcall__ PrintStringFromDataBuffer();

//
// asm-conio.s
//
void __fastcall__ cputchar_direct(char c);
void __fastcall__ fillchar_direct(char c,uint8_t count);
char __fastcall__ cgetchar(char cursor);
void __fastcall__ newline();
void __fastcall__ newline2();
void __fastcall__ newlinex(uint8_t x);
void __fastcall__ cursordown();
void __fastcall__ cursorup();
void __fastcall__ cursorupx(uint8_t xpos);
void __fastcall__ cursorupx_m1(uint8_t xpos);
void __fastcall__ cursorleft();
void __fastcall__ gotoxy00();
void __fastcall__ clreol();
void __fastcall__ setwndlft(uint8_t x);
void __fastcall__ resetwndlft();

//
// asm.s
//
void __fastcall__ Reboot();
uint8_t __fastcall__ IsAppleIIcplus();
void __fastcall__ ToUppercase(char*);
void __fastcall__ ZeroMemory(uint8_t len,void* dest);
bool __fastcall__ HasFPUSupport();
bool __fastcall__ ReadOpenAppleButton();





void __fastcall__ Send_CMD_TFTPRUN();
int8_t  __fastcall__ GetParam8();
int16_t __fastcall__ GetParam16();

#endif

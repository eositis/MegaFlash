#ifndef _ASM_H
#define _ASM_H

#include <stdint.h>

uint8_t __fastcall__ GetBoardType();
uint8_t __fastcall__ GetUnitCount();
uint8_t __fastcall__ DisableROMDisk();
bool __fastcall__ GetVolInfo(uint8_t unitNum); 
bool __fastcall__ SaveSetting(uint8_t cmd, uint8_t len, void* src);
void __fastcall__ LoadSetting(uint8_t cmd, uint8_t len, void* dest);
uint8_t __fastcall__ FormatDisk();
void __fastcall__ DisplayTime();
void __fastcall__ Reboot();
uint8_t __fastcall__ TestWifi();
void __fastcall__ EraseAllConfig(); 
uint16_t __fastcall__ GetUnitBlockCount(uint8_t unitNum);
void __fastcall__ PrintIPAddrFromDataBuffer();
uint8_t __fastcall__ IsAppleIIcplus();

void __fastcall__ cputchar(char c);
char __fastcall__ cgetchar(char cursor);
void __fastcall__ fillchar(char c,uint8_t count);
void __fastcall__ newline();
void __fastcall__ newline2();
void __fastcall__ newlinex(uint8_t x);
void __fastcall__ cursordown();
void __fastcall__ cursorup();
void __fastcall__ cursorupx();
void __fastcall__ cursorleft();

void __fastcall__ setwndlft(uint8_t x);
void __fastcall__ resetwndlft();

void __fastcall__ ToUppercase(char*);

#endif

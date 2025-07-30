#ifndef _FLASHUNITMAPPER_H
#define _FLASHUNITMAPPER_H


void SetupFlashUnitMapping();
uint32_t GetUnitCountFlashEnabled();
uint32_t MapFlashUnitNum(uint32_t logicalUnitNum);
void EnableFlashUnitMapping();
void DisableFlashUnitMapping();

#endif
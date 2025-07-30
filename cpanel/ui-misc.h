#ifndef _UI_MISC_H
#define _UI_MISC_H

char cgetc_showclock();
bool AskUserToConfirm();
void PrintDriveInfoList(uint8_t unitCount);
void PrintDriveList(uint8_t unitCount);
void PrintVolumeType(uint8_t type);
void PrintDriveInfo(uint8_t unit);
void PrintStringTwoLines(char* s,uint8_t width);
void cputs_n(char *s,uint8_t num);
void ResetScreen();

#endif

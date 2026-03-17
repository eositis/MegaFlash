#ifndef _UI_MISC_H
#define _UI_MISC_H

char cgetc_showclock();
uint8_t GetDriveListCount(void);  /* Unit count excluding ROM disk when last */
bool AskUserToConfirm();
void PrintDriveInfoList(uint8_t unitCount);
void PrintDriveList(uint8_t unitCount);
/* Same as PrintDriveList but draws checkboxes inline for alignment. enableFlags/ramdiskFlag may be NULL/false if not needed. */
void PrintDriveListWithCheckboxes(uint8_t unitCount, const bool *enableFlags, bool ramdiskFlag);
void PrintVolumeType(uint8_t type);
void PrintDriveInfo(uint8_t unit);
void PrintStringTwoLines(char* s,uint8_t width);
void cputs_n(char *s,uint8_t num);
void ResetScreen();

#endif

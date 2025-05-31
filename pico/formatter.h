#ifndef _FORMATTER_H
#define _FORMATTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "defines.h"


uint32_t SanitizeVolumeName(char* volName);
bool FormatUnit(const uint32_t unitNum,const uint16_t blockCount,const char *volName,const uint32_t volNameLen);

#ifdef __cplusplus
}
#endif

#endif
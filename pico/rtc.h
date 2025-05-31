#ifndef _RTC_H
#define _RTC_H

#ifdef __cplusplus
extern "C" {
#endif

#include  <time.h>

void InitRTC(const time_t secondsSince1970,const int32_t offset);
bool IsRTCRunning();
void SetNewTimezoneOffset(const int32_t newOffset);
void GetProdosTimestamp(uint8_t *timestamp);
void GetProdos25Timestamp(uint8_t *timestamp);
void SetRTCFromProdosTimestamp(uint8_t *timestamp);
void SetRTCFromProdos25Timestamp(uint8_t *timestamp);

#ifdef __cplusplus
}
#endif

#endif


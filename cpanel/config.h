#ifndef _CONFIG_H
#define _CONFIG_H
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"

void InitConfig();
bool ValidateConfig(UserConfig_t* config);
void LoadConfig();
void SaveConfigReboot();

#endif

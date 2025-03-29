#ifndef _CONFIG_H
#define _CONFIG_H
#include <stdint.h>
#include <stdbool.h>
#include "defines.h"

void InitConfig();
bool ValidateConfig(ConfigApple* config);
void LoadConfig();
void SaveConfig();

#endif

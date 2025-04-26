#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "defines.h"
#include "asm.h"

//From main.c
extern UserConfig_t config;
extern bool isAppleIIe;

/////////////////////////////////////////////////////////////////////
// Validate User Config
//
// Output: true if config is valid.
//
static bool ValidateConfig() {
  if (config.version!=USERCONFIGVER || (config.version^USERCONFIG_CHKBYTECOMP)!=config.checkbyte || config.timezoneidver!=TIMEZONEIDVER) {
    return false;
  }
  return true;
}

#ifdef TESTBUILD
void InitConfig() {
  config.version     = USERCONFIGVER;
  config.checkbyte   = USERCONFIGVER ^ USERCONFIG_CHKBYTECOMP;
  config.configbyte1 = DEFCFGBYTE1;  
  config.configbyte2 = DEFCFGBYTE2;
  config.timezoneidver = TIMEZONEIDVER;
  config.timezoneid  = DEFAULTTIMEZONE;
}
#endif  

/////////////////////////////////////////////////////////////////////
// Load User Config from MegaFlash
//
void LoadConfig() {
#ifdef TESTBUILD
  InitConfig();
  return;
#endif  
  
  LoadSetting(CMD_GETUSERCONFIG, sizeof(UserConfig_t), &config);
  
  //Validate it
  if (!ValidateConfig()) {
    FatalError(ERR_CONFIG_INVALID);
  }
}


/////////////////////////////////////////////////////////////////////
// Save User Config to MegaFlash and then Reboot
//
void SaveConfigReboot() {
  static_local bool success;
  
  //Check version, checkbyte and zoneverid
  if (!ValidateConfig()) {
    FatalError(ERR_CONFIG_INVALID);
  }
 
  //Save to MegaFlash
  success = SaveSetting(CMD_SAVEUSERCONFIG,(uint8_t)sizeof(UserConfig_t),&config);  
  
  //We don't expect the saving would fail. So, no need to provide a fancy UI.
  if (!success) {
    FatalError(ERR_SaveConfigReboot_FAIL);
  }
  Reboot();
}
#ifndef _common_defines_h
#define _common_defines_h

#include <stdint.h>

//This file is shared with Pico and CPanel project

//************** MegaFlash ********************/
//MegaFlash Commands
#define CMD_RESETBOTHPTRS       0x00
#define CMD_RESETDATAPTR        0x01
#define CMD_RESETPARAMPTR       0x02
#define CMD_MODELINEAR          0x03
#define CMD_MODEINTERLEAVED     0x04
#define CMD_COLDSTART           0x05
#define CMD_ENABLEROMDISK       0x06
#define CMD_DISABLEROMDISK      0x07
#define CMD_LOAD_CPANEL         0x08
#define CMD_TESTWIFI            0x09

#define CMD_RESETTIMER_US       0x0A
#define CMD_GETTIMER_US         0x0B
#define CMD_RESETTIMER_MS       0x0C
#define CMD_GETTIMER_MS         0x0D
#define CMD_RESETTIMER_S        0x0E
#define CMD_GETTIMER_S          0x0F

#define CMD_GETDEVINFO          0x10
#define CMD_GETDEVSTATUS        0x11
#define CMD_GETUNITSTATUS       0x12
#define CMD_GETDIB              0x13
#define CMD_GETVOLINFO          0x14
#define CMD_READBLOCK           0x15
#define CMD_WRITEBLOCK          0x16
#define CMD_GETPRODOSTIME       0x17
#define CMD_GETPRODS25TIME      0x18  
#define CMD_GETTIMESTR          0x19
#define CMD_SETRTC_PRODOS       0x1a
#define CMD_SETRTC_PRODOS25     0x1b
#define CMD_WRITEBLOCKSIZETOVDH 0x1c
#define CMD_FORMATDISK          0x1d


#define CMD_SAVEUSERCONFIG      0x20
#define CMD_GETUSERCONFIG       0x21
#define CMD_SAVEWIFISETTING     0x22
#define CMD_GETCONFIGBYTES      0x23
#define CMD_ERASEUSERCONFIG     0x24
#define CMD_ERASEWIFISETTING    0x25
#define CMD_ERASEALLCONFIG      0x26

#define CMD_FADD                0x30
#define CMD_FMUL                0x31
#define CMD_FDIV                0x32
#define CMD_FSIN                0x33
#define CMD_FCOS                0x34
#define CMD_FTAN                0x35
#define CMD_FATN                0x36
#define CMD_FLOG                0x37
#define CMD_FEXP                0x38
#define CMD_FSQR                0x39
#define CMD_FOUT                0x3a
#define CMD_FMUL10              0x3b
#define CMD_FDIV10              0x3c

//MegaFlash Error Code
#define ERR_SUCCESS      0x00  /* No Error*/
#define ERR_NOFLASH      0x01  /* Supported Flash Chip is not found  */
#define ERR_UNKNOWNCMD   0x02  /* Unknown Command Code */
#define ERR_INVALIDWEKEY 0x03  /* Invalid Write Enable KEY */
#define ERR_INVALIDUNIT  0x04  /* Invalid Unit Number */
#define ERR_INVALIDBLK   0x05  /* Invalid Block Number */
#define ERR_RWERROR      0x06  /* ReadBlock/WriteBlock Error */
#define ERR_INVALIDPAGE  0x07  /* LoadCPanel Invalid Page */
#define ERR_USERCONFIG   0x08  /* Error in accessing User Configuration */

//Write Enable Key
#define WRITEENABLEKEY  0x71

//Status Registers Fields
#define BUSYFLAG       0b10000000
#define ERRORFLAG      0b01000000
#define ERRORCODEFIELD 0b00001111 /*Last four bits are error code field*/

//Board Type
//MSB is set if Wifi is supported
typedef enum {
  BRD_PICO   = 0,
  BRD_PICOW  = BRD_PICO  | 0x80,
  BRD_PICO2  = 1,
  BRD_PICO2W = BRD_PICO2 | 0x80
} BoardType;




//***************** User Preference *******************

//This data structure is uploaded to dataBuffer by Control Panel App
#define SSIDLEN         32
#define WPAKEYLEN       63
#define WIFISETTINGVER  0
#define WIFIAUTHTYPE    0
typedef struct {
  uint8_t version;          //Version ID of the structure
  uint8_t authType;         //Authentication Type. Currently unused and should be 0
  char wpakey[WPAKEYLEN+1]; //Wifi Password. +1 for null character  
  char ssid[SSIDLEN+1];     //SSID. +1 for null character
} WifiSettingApple;


//This data structure is written to flash memory.
typedef struct {
  uint32_t magic;
  
  /**** The following fields must match with WifiSettingApple ****/
  uint8_t version;          //Version ID of the structure
  uint8_t authType;         //Authentication Type. Currently unused and should be 0
  char wpakey[WPAKEYLEN+1]; //Wifi Password. +1 for null character 
  char ssid[SSIDLEN+1];     //SSID. +1 for null character
  /***************************************************************/

  uint8_t padding;          //To make the size multiple of 4
} WifiSettingPico;
#define WIFISETTINGOFFSET   4   //Offset of WifiSettingApple data field

//****************************************

//
//Machine Configuration
#define USERCONFIGVER   0
#define TIMEZONEIDVER   0
#define CHKBYTECOMP     0x5A

//Bits definition of configbyte1
#define CPUSPEEDFLAG   0b10000000 //1=Normal(1MHz), 0=Fast
#define AUTOBOOTFLAG   0b01000000
#define RAMDISKFLAG    0b00100000
#define NTPCLIENTFLAG  0b00010000
#define FPUFLAG        0b00001000
#define DEFCFGBYTE1    0b01000000  //Default configbyte1 value
#define DEFCFGBYTE2    0           //Default configbyte2 value

typedef struct  {
  uint8_t  version;
  uint8_t  checkbyte;   //=version^CHKBYTECOMP to indicate the structure data is valid    
  uint8_t  configbyte1;
  uint8_t  configbyte2; //=0, reserved for future
  uint8_t  timezoneidver;  
  uint8_t  timezoneid;
} ConfigApple;

typedef struct {
  uint32_t magic;
  
  /****** The following fields must match with ConfigApple ******/  
  uint8_t  version;
  uint8_t  checkbyte;   //=version^$5A to indicate the structure data is valid  
  uint8_t  configbyte1;
  uint8_t  configbyte2; //=0, reserved for future
  uint8_t  timezoneidver;  
  uint8_t  timezoneid;  
  /***************************************************************/  
  
  uint8_t  padding[2];
} ConfigPico;
#define USERCONFIGOFFSET 4  //Offset of ConfigApple data field


/********* Time Zone Info **************/

#define TZHOUR {-12,-11,-10,-9,-9,-8,-7,-6,-5,-4,-3,-3,-2,-1,0,1,2,3, 3,4, 4,5, 5, 5,6, 6,7,8, 8,9, 9,10,10,11,12,12,13,14}
#define TZMIN  {  0,  0,  0,30, 0, 0, 0, 0, 0, 0,30, 0, 0, 0,0,0,0,0,30,0,30,0,30,45,0,30,0,0,45,0,30, 0,30, 0, 0,45, 0, 0}
#define DEFAULTTIMEZONE 14  //UTC+0:00


/******************************* Test Wifi *********************************/
typedef enum {
  ERR_UNKNOWN,
  ERR_NOTPICOW,
  ERR_NETTIMEOUT,
  ERR_SSIDNOTSET,
  ERR_NONET,
  ERR_WIFINOTCONNECTED,
  ERR_BADAUTH,  
  ERR_NOIP,     //Wifi OK, DHCP problem
  ERR_DNSFAILED,//Wifi OK, DHCP OK, DNS problem
  ERR_NTPFAILED,//Wifi OK, DHCP OK, DNS OK, NTP problem
  ERR_NOERR     //Everything ok
} NetworkError_t;


#endif
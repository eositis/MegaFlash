#ifndef _megaflash_defines_h
#define _megaflash_defines_h

#include "hardware/gpio.h"
#include "../common/defines.h"

/****************************************************

     Constants

****************************************************/

#define FIRMWAREVER     0x0021
#define FIRMWAREVERSTR  "V1.2.1-eo"
// Per-build id is NOT here: CMake generates build_id.h (Unix + human-readable UTC string; see build_id.h.in, build-both.sh).
// 0x0000 = V0.1
// 0x0001 = V0.2   18-Apr-2025
// 0x0002 = V0.3   05-May-2025
// 0x0003 = V1.0   14-Jul-2025
// 0x0004 = V1.1   30-Jul-2025
// 0x0005 = V1.1.1 04-Aug-2025
// 0x0006 = V1.1.2 11-Aug-2025
// 0x0007 = V1.1.3 15-Aug-2025
// 0x0008 = V1.1.4 18-Aug-2025
// 0x0009 = V1.1.5 25-Aug-2025
// 0x000a = V1.1.6-eo 02-Mar-2026
// 0x000b = V1.1.7-eo 02-Mar-2026
// 0x000c = V1.1.8-eo 05-Mar-2026
// 0x000d = V1.1.9-eo 08-Mar-2026
// 0x000e = V1.1.10-eo 08-Mar-2026
// 0x000f = V1.1.11-eo 08-Mar-2026
// 0x0010 = V1.1.12-eo 08-Mar-2026
// 0x0011 = V1.1.13-eo 08-Mar-2026
// 0x0012 = V1.1.14-eo 08-Mar-2026
// 0x0013 = V1.1.15-eo 09-Mar-2026
// 0x0014 = V1.1.16-eo 09-Mar-2026
// 0x0015 = V1.1.17-eo 09-Mar-2026
// 0x0016 = V1.1.18-eo 09-Mar-2026
// 0x0017 = V1.1.19-eo 17-Mar-2026
// 0x0018 = V1.1.20-eo 17-Mar-2026
// 0x0019 = V1.1.21-eo 18-Mar-2026
// 0x001a = V1.1.22-eo 18-Mar-2026
// 0x001b = V1.1.23-eo 18-Mar-2026
// 0x001c = V1.1.24-eo 21-Mar-2026  (last 1.1.x maintenance release)
// 0x0020 = V1.2.0-eo 21-Mar-2026  (1.2.x series: Uthernet II, com port, imagewriter)
// 0x0021 = V1.2.1-eo 26-Apr-2026

//Deivce Signature Bytes
#define SIGNATURE1 0x88
#define SIGNATURE2 0x74

//IDREG Initial Value
#define IDREG_VAL 0x96

//
//Formatter
//
#define FMT_MINBLOCKCOUNT 32
#define FMT_MAXBLOCKCOUNT 0xffff
#define FMT_VOLNAMEMAXLEN 15
#define FMT_DEFAULTVOLNAME "MEGAFLASH"


//RAM Disk Size in Bytes
#ifdef PICO_RP2040
#define RAMDISK_SIZE (140*1024)
#else
#define RAMDISK_SIZE (256*1024)   /* RP2350: was 400 KiB; smaller leaves more heap for WiFi/TFTP */
#endif

//Slinky Size in Bytes
#ifdef PICO_RP2040
#define SLINKY_SIZE (128*1024)
#else
#define SLINKY_SIZE (256*1024)
#endif

//Buffer Size
#define PARAMBUFFERSIZE  32 //Note: Smartport DIB requires 25 bytes
#define PARAMBUFFERINDEXMASK 0b11111
#define DATABUFFERSIZE   512
#define DATABUFFERINDEXMASK  0b111111111
#define BLOCKSIZE        512
#define PAGESIZE         256

//////////////////////////////////////////////////////////////////


//I/O Pins
#define PHI0_PIN 19
#define nRESET_PIN   21  /* Apple /Reset PIN, active low */
#define ACT_LED_PIN  26
#define PICO_LED_PIN 25  /* Pico Onboard LED */

/* Address decode only (no GPIO slot select): C0x0–C0x3 = MegaFlash; C0x4–C0x7 only = Uthernet II W5100 ($C0C4–$C0C7). $C0C8–$C0CF are not U2.
 * The bus is presented when the card’s slot is addressed; no pin 27 or other slot-select input. */
#define U2_C0X_OFFSET     4   /* first Uthernet II address */
#define U2_C0X_LAST       7   /* last Uthernet II address ($C0C4–$C0C7 only) */

static inline void TurnOnActLed() {
  gpio_clr_mask(1ul<<ACT_LED_PIN); //Turn on
}

static inline void TurnOffActLed() {
  gpio_set_mask(1ul<<ACT_LED_PIN); //Turn off
}



//Data Buffer Transfer Mode
typedef enum {
  MODE_LINEAR,
  MODE_INTERLEAVED
}transfermode_t;
#define DEFAULTTRANSFERMODE MODE_LINEAR

//MegaFlash Registers Address
#define CMDREG    0 /*$C0C0*/
#define STATUSREG 0 /*$C0C0*/
#define PARAMREG  1 /*$C0C1*/
#define DATAREG   2 /*$C0C2*/
#define IDREG     3 /*$C0C3*/



//ProDOS/SmartPort ReadBlock/WriteBlock Error Code
typedef enum {
  SP_NOERR      = 0,      //No Error
  SP_IOERR      = 0x27,   //I/O Error
  SP_NODRVERR   = 0x28,   //No Device Connected
  SP_NOWRITEERR = 0x2B,   //Write Protected Error
}rwerror_t;



#endif
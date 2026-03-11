/* MegaFlash cc65 library - public API
 *
 * Target: Apple IIc / IIc+ with MegaFlash installed
 *
 * This header provides a small, C-friendly wrapper around the
 * $C0C0 MegaFlash command interface. It is designed for cc65
 * programs using the apple2 or apple2enh targets.
 *
 * Usage (single source file):
 *   cl65 -t apple2 -O -o myprog myprog.c cc65/megaflash.c
 */

#ifndef MEGAFLASH_H
#define MEGAFLASH_H

#include <stdint.h>

/* --------------------------------------------------------------------
 * Low-level register addresses
 * ------------------------------------------------------------------ */

#define MF_CMDREG   (*(volatile uint8_t*)0xC0C0u)
#define MF_STATUS   (*(volatile uint8_t*)0xC0C0u)
#define MF_PARAM    (*(volatile uint8_t*)0xC0C1u)
#define MF_DATA     (*(volatile uint8_t*)0xC0C2u)
#define MF_ID       (*(volatile uint8_t*)0xC0C3u)

/* Status bits (read from MF_STATUS) */
#define MF_BUSYFLAG       0x80u
#define MF_ERRORFLAG      0x40u
#define MF_ERRORCODE_MASK 0x1Fu

/* Write-enable key required for destructive operations */
#define MF_WE_KEY         0x71u

/* --------------------------------------------------------------------
 * Command codes (subset)
 * ------------------------------------------------------------------ */

enum {
    MF_CMD_RESETBOTHPTRS       = 0x00,
    MF_CMD_GETINFOSTR          = 0x0A,

    MF_CMD_GETDEVINFO          = 0x10,
    MF_CMD_GETDEVSTATUS        = 0x11,
    MF_CMD_GETUNITSTATUS       = 0x12,
    MF_CMD_GETDIB              = 0x13,
    MF_CMD_GETVOLINFO          = 0x14,
    MF_CMD_READBLOCK           = 0x15,
    MF_CMD_WRITEBLOCK          = 0x16,
    MF_CMD_GETPRODOSTIME       = 0x17,
    MF_CMD_GETPRODOS25TIME     = 0x18,
    MF_CMD_GETTIMESTR          = 0x19,
    MF_CMD_SETRTC_PRODOS       = 0x1A,
    MF_CMD_SETRTC_PRODOS25     = 0x1B,
    MF_CMD_WRITEBLOCKSIZETOVDH = 0x1C,
    MF_CMD_FORMATDISK          = 0x1D,
    MF_CMD_ERASEDISK           = 0x1E,

    MF_CMD_SAVEUSERSETTINGS    = 0x20,
    MF_CMD_GETUSERSETTINGS     = 0x21,
    MF_CMD_SAVEWIFISETTINGS    = 0x22,
    MF_CMD_GETCONFIGBYTES      = 0x23,
    MF_CMD_ERASEUSERSETTINGS   = 0x24,
    MF_CMD_ERASEWIFISETTINGS   = 0x25,
    MF_CMD_ERASEADVSETTINGS    = 0x26,
    MF_CMD_ERASEALLSETTINGS    = 0x27,
    MF_CMD_DRIVEMAPPING        = 0x28,
    MF_CMD_GETFIRMWAREVER      = 0x29,

    MF_CMD_RESETTIMER_US       = 0x40,
    MF_CMD_GETTIMER_US         = 0x41,
    MF_CMD_RESETTIMER_MS       = 0x42,
    MF_CMD_GETTIMER_MS         = 0x43,
    MF_CMD_RESETTIMER_S        = 0x44,
    MF_CMD_GETTIMER_S          = 0x45,

    MF_CMD_TFTPRUN             = 0x50,
    MF_CMD_TFTPSTATUS          = 0x51,

    MF_CMD_ENABLEROMDISK       = 0x06,
    MF_CMD_DISABLEROMDISK      = 0x07,
    MF_CMD_TESTWIFI            = 0x09
};

/* --------------------------------------------------------------------
 * Simple error handling
 * ------------------------------------------------------------------ */

/* Last Pico error code from a MegaFlash call (bits 0–4 of MF_STATUS). */
extern uint8_t mf_last_error;

/* Convenience: return non-zero if last call failed. */
uint8_t __fastcall__ mf_failed(void);

/* --------------------------------------------------------------------
 * Structures
 * ------------------------------------------------------------------ */

/* Compact view of the 21-byte VolInfo_t structure used by firmware. */
typedef struct mf_volinfo {
    uint8_t  type;          /* 0 = ProDOS, etc. */
    uint16_t block_count;   /* 16-bit view of block count (LSB first) */
    uint8_t  medium;        /* flash / RAM / ROM code */
    uint8_t  name_len;      /* number of chars in name */
    char     name[16];      /* null-terminated volume name (max 15 chars) */
} mf_volinfo_t;

/* --------------------------------------------------------------------
 * Core API
 * ------------------------------------------------------------------ */

/* Return number of SmartPort units, or 0xFF on error (see mf_last_error). */
uint8_t __fastcall__ mf_get_unit_count(void);

/* Fill mf_volinfo_t for unitNum. Returns 0 on failure, non-zero on success. */
uint8_t __fastcall__ mf_get_volinfo(uint8_t unitNum, mf_volinfo_t* info);

/* Read one 512-byte block. Returns 0 on success; non-zero error code. */
uint8_t __fastcall__ mf_read_block(uint8_t unitNum,
                                   uint32_t block,
                                   void*    buffer);

/* Write one 512-byte block. Returns 0 on success; non-zero error code. */
uint8_t __fastcall__ mf_write_block(uint8_t unitNum,
                                    uint32_t block,
                                    const void* buffer);

/* Get firmware version string (12 chars, not high-bit set). */
void __fastcall__ mf_get_firmware_version(char* out12);

/* Get time string (8 chars, e.g. "11:50 AM"). */
void __fastcall__ mf_get_time_string(char* out8);

/* Enable ROM disk as last unit. Returns 0 on success. */
uint8_t __fastcall__ mf_enable_romdisk_last(void);

/* Enable ROM disk as first unit (boot). Returns 0 on success. */
uint8_t __fastcall__ mf_enable_romdisk_first(void);

/* Disable ROM disk entirely. Returns 0 on success. */
uint8_t __fastcall__ mf_disable_romdisk(void);

/* Run WiFi self-test. Returns Pico error code (0 = OK). */
uint8_t __fastcall__ mf_test_wifi(void);

#endif /* MEGAFLASH_H */


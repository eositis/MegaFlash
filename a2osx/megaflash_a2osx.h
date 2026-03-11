/* MegaFlash library for A2osX C
 *
 * This header describes a small C API for calling the MegaFlash
 * $C0C0 command interface from the C compiler used by A2osX.
 *
 * It is intentionally conservative (K&R-style, no stdint.h, no
 * compiler-specific attributes) so it can be compiled by A2osX CC.
 *
 * See docs/a2osx-megaflash-lib.md for usage and examples.
 */

#ifndef MEGAFLASH_A2OSX_H
#define MEGAFLASH_A2OSX_H

/* Basic integer types (avoid stdint.h for portability). */
typedef unsigned char  mf_u8;
typedef unsigned short mf_u16;
typedef unsigned long  mf_u32;

/* --------------------------------------------------------------------
 * Hardware registers
 * ------------------------------------------------------------------ */

#define MF_CMDREG   (*(mf_u8*)0xC0C0u)
#define MF_STATUS   (*(mf_u8*)0xC0C0u)
#define MF_PARAM    (*(mf_u8*)0xC0C1u)
#define MF_DATA     (*(mf_u8*)0xC0C2u)
#define MF_ID       (*(mf_u8*)0xC0C3u)

/* Status bits (read from MF_STATUS) */
#define MF_BUSYFLAG       0x80u
#define MF_ERRORFLAG      0x40u
#define MF_ERRORCODE_MASK 0x1Fu

/* Write-enable key required for destructive operations */
#define MF_WE_KEY         0x71u

/* Command codes (subset) */
#define MF_CMD_RESETBOTHPTRS       0x00u
#define MF_CMD_GETINFOSTR          0x0Au

#define MF_CMD_GETDEVINFO          0x10u
#define MF_CMD_GETDEVSTATUS        0x11u
#define MF_CMD_GETUNITSTATUS       0x12u
#define MF_CMD_GETDIB              0x13u
#define MF_CMD_GETVOLINFO          0x14u
#define MF_CMD_READBLOCK           0x15u
#define MF_CMD_WRITEBLOCK          0x16u
#define MF_CMD_GETPRODOSTIME       0x17u
#define MF_CMD_GETPRODOS25TIME     0x18u
#define MF_CMD_GETTIMESTR          0x19u
#define MF_CMD_SETRTC_PRODOS       0x1Au
#define MF_CMD_SETRTC_PRODOS25     0x1Bu
#define MF_CMD_WRITEBLOCKSIZETOVDH 0x1Cu
#define MF_CMD_FORMATDISK          0x1Du
#define MF_CMD_ERASEDISK           0x1Eu

#define MF_CMD_SAVEUSERSETTINGS    0x20u
#define MF_CMD_GETUSERSETTINGS     0x21u
#define MF_CMD_SAVEWIFISETTINGS    0x22u
#define MF_CMD_GETCONFIGBYTES      0x23u
#define MF_CMD_ERASEUSERSETTINGS   0x24u
#define MF_CMD_ERASEWIFISETTINGS   0x25u
#define MF_CMD_ERASEADVSETTINGS    0x26u
#define MF_CMD_ERASEALLSETTINGS    0x27u
#define MF_CMD_DRIVEMAPPING        0x28u
#define MF_CMD_GETFIRMWAREVER      0x29u

#define MF_CMD_RESETTIMER_US       0x40u
#define MF_CMD_GETTIMER_US         0x41u
#define MF_CMD_RESETTIMER_MS       0x42u
#define MF_CMD_GETTIMER_MS         0x43u
#define MF_CMD_RESETTIMER_S        0x44u
#define MF_CMD_GETTIMER_S          0x45u

#define MF_CMD_TFTPRUN             0x50u
#define MF_CMD_TFTPSTATUS          0x51u

#define MF_CMD_ENABLEROMDISK       0x06u
#define MF_CMD_DISABLEROMDISK      0x07u
#define MF_CMD_TESTWIFI            0x09u

/* --------------------------------------------------------------------
 * Error state
 * ------------------------------------------------------------------ */

/* Last Pico error code from a MegaFlash call (bits 0–4 of MF_STATUS). */
extern mf_u8 mf_last_error;

/* Convenience: return non-zero if last call failed. */
mf_u8 mf_failed(void);

/* --------------------------------------------------------------------
 * Structures
 * ------------------------------------------------------------------ */

typedef struct mf_volinfo {
    mf_u8  type;          /* 0 = ProDOS, etc. */
    mf_u16 block_count;   /* 16-bit view of block count (LSB first) */
    mf_u8  medium;        /* flash / RAM / ROM code */
    mf_u8  name_len;      /* number of chars in name */
    char   name[16];      /* null-terminated volume name (max 15 chars) */
} mf_volinfo_t;

/* --------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/* Return number of SmartPort units, or 0xFF on error (see mf_last_error). */
mf_u8 mf_get_unit_count(void);

/* Fill mf_volinfo_t for unitNum. Returns 0 on failure, non-zero on success. */
mf_u8 mf_get_volinfo(mf_u8 unitNum, mf_volinfo_t* info);

/* Read one 512-byte block. Returns 0 on success; non-zero error code. */
mf_u8 mf_read_block(mf_u8 unitNum, mf_u32 block, void* buffer);

/* Write one 512-byte block. Returns 0 on success; non-zero error code. */
mf_u8 mf_write_block(mf_u8 unitNum, mf_u32 block, const void* buffer);

/* Get firmware version string (12 chars, not terminated). */
void mf_get_firmware_version(char* out12);

/* Get time string (8 chars, e.g. "11:50 AM", not terminated). */
void mf_get_time_string(char* out8);

/* ROM disk control */
mf_u8 mf_enable_romdisk_last(void);
mf_u8 mf_enable_romdisk_first(void);
mf_u8 mf_disable_romdisk(void);

/* WiFi self-test. Returns test result code (0 = OK). */
mf_u8 mf_test_wifi(void);

#endif /* MEGAFLASH_A2OSX_H */


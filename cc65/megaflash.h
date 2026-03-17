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

    /* FPU operations (Applesoft-compatible) */
    MF_CMD_FADD                = 0x30,
    MF_CMD_FMUL                = 0x31,
    MF_CMD_FDIV                = 0x32,
    MF_CMD_FSIN                = 0x33,
    MF_CMD_FCOS                = 0x34,
    MF_CMD_FTAN                = 0x35,
    MF_CMD_FATN                = 0x36,
    MF_CMD_FLOG                = 0x37,
    MF_CMD_FEXP                = 0x38,
    MF_CMD_FSQR                = 0x39,
    MF_CMD_FOUT                = 0x3A,

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

/* FPU argument / result buffers (MBF-compatible layout)
 *
 * The Pico FPU expects 13 bytes of FAC/ARG data in the parameter buffer:
 *
 *   bytes[ 0] = FACSIGN    ($A2)
 *   bytes[ 1] = ARGSIGN    ($AA)
 *   bytes[ 2] = FACMANT4   ($A1)
 *   bytes[ 3] = ARGMANT4   ($A9)
 *   bytes[ 4] = FACMANT3   ($A0)
 *   bytes[ 5] = ARGMANT3   ($A8)
 *   bytes[ 6] = FACMANT2   ($9F)
 *   bytes[ 7] = ARGMANT2   ($A7)
 *   bytes[ 8] = FACMANT1   ($9E)
 *   bytes[ 9] = ARGMANT1   ($A6)
 *   bytes[10] = FACEXP     ($9D)
 *   bytes[11] = ARGEXP     ($A5)
 *   bytes[12] = FACEXT     ($AC)
 *
 * See pico/fpu.c comments for details of the MBF format.
 */
typedef struct mf_fpu_args {
    uint8_t bytes[13];
} mf_fpu_args_t;

/* FPU result: 1-byte error code + 7-byte MBF value.
 *
 * Layout matches the Pico's StoreResult() output:
 *
 *   error     = bytes[0]
 *   bytes[1]  = sign
 *   bytes[2]  = mantissa4
 *   bytes[3]  = mantissa3
 *   bytes[4]  = mantissa2
 *   bytes[5]  = mantissa1 (MSB always set)
 *   bytes[6]  = exponent
 *   bytes[7]  = extension
 */
typedef struct mf_fpu_result {
    uint8_t bytes[8];
} mf_fpu_result_t;

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

/* --------------------------------------------------------------------
 * FPU API (MBF-level)
 *
 * These functions expose MegaFlash's FPU operations at the MBF data
 * level. The caller is responsible for converting between C floating
 * point types and Applesoft MBF format if needed.
 *
 * For all functions:
 *   - 'args' must point to 13 bytes of FAC/ARG as described above.
 *   - 'res' must point to an 8-byte buffer that will receive:
 *       res->bytes[0] = error flags (overflow/div0/illegal quantity)
 *       res->bytes[1..7] = MBF result value.
 *   - mf_last_error is set to a non-zero Pico error code if the
 *     MegaFlash command itself fails; otherwise it remains 0.
 * ------------------------------------------------------------------ */

void __fastcall__ mf_fpu_op(uint8_t cmd,
                            const mf_fpu_args_t* args,
                            mf_fpu_result_t* res);

void __fastcall__ mf_fadd(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fmul(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fdiv(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fsin(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fcos(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_ftan(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fatn(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_flog(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fexp(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fsqr(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void __fastcall__ mf_fout(const mf_fpu_args_t* args, mf_fpu_result_t* res);

#endif /* MEGAFLASH_H */


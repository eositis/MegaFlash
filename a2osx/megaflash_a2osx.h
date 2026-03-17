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

/* FPU operations (Applesoft-compatible) */
#define MF_CMD_FADD                0x30u
#define MF_CMD_FMUL                0x31u
#define MF_CMD_FDIV                0x32u
#define MF_CMD_FSIN                0x33u
#define MF_CMD_FCOS                0x34u
#define MF_CMD_FTAN                0x35u
#define MF_CMD_FATN                0x36u
#define MF_CMD_FLOG                0x37u
#define MF_CMD_FEXP                0x38u
#define MF_CMD_FSQR                0x39u
#define MF_CMD_FOUT                0x3Au

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

/* FPU argument / result buffers (MBF-compatible layout)
 *
 * See docs/cc65-megaflash-lib.md and pico/fpu.c for detailed comments
 * on the MBF format and the byte ordering used by the firmware.
 */
typedef struct mf_fpu_args {
    mf_u8 bytes[13];
} mf_fpu_args_t;

typedef struct mf_fpu_result {
    mf_u8 bytes[8];   /* [0] = error flags, [1..7] = MBF result */
} mf_fpu_result_t;

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

/* --------------------------------------------------------------------
 * FPU API (MBF-level)
 *
 * These calls send 13 bytes of FAC/ARG state to MegaFlash and return
 * an 8-byte MBF result. The C caller is responsible for converting
 * between its own float format and MBF if needed.
 * ------------------------------------------------------------------ */

void mf_fpu_op(mf_u8 cmd,
               const mf_fpu_args_t* args,
               mf_fpu_result_t*     res);

void mf_fadd(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fmul(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fdiv(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fsin(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fcos(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_ftan(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fatn(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_flog(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fexp(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fsqr(const mf_fpu_args_t* args, mf_fpu_result_t* res);
void mf_fout(const mf_fpu_args_t* args, mf_fpu_result_t* res);

#endif /* MEGAFLASH_A2OSX_H */


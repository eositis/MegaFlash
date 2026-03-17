/* MegaFlash cc65 library implementation
 *
 * See docs/cc65-megaflash-lib.md for a human-readable reference.
 */

#include <stdint.h>
#include "megaflash.h"

uint8_t mf_last_error = 0;

/* --------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */

/* Wait until BUSY bit clears. */
static void mf_wait_busy(void)
{
    while (MF_STATUS & MF_BUSYFLAG) {
        /* spin */
    }
}

/* Issue a command byte and update mf_last_error. */
static uint8_t mf_issue_cmd(uint8_t cmd)
{
    MF_CMDREG = cmd;
    mf_wait_busy();
    if (MF_STATUS & MF_ERRORFLAG) {
        mf_last_error = MF_STATUS & MF_ERRORCODE_MASK;
    } else {
        mf_last_error = 0;
    }
    return mf_last_error;
}

uint8_t __fastcall__ mf_failed(void)
{
    return (mf_last_error != 0);
}

/* --------------------------------------------------------------------
 * Core API
 * ------------------------------------------------------------------ */

uint8_t __fastcall__ mf_get_unit_count(void)
{
    /* Just CMD_GETDEVSTATUS; first param byte is unit count. */
    mf_issue_cmd(MF_CMD_GETDEVSTATUS);
    if (mf_last_error != 0) {
        return 0xFFu;
    }
    return MF_PARAM;
}

uint8_t __fastcall__ mf_get_volinfo(uint8_t unitNum, mf_volinfo_t* info)
{
    uint8_t i;

    if (!info) {
        return 0;
    }

    /* Reset both pointers. */
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* param0 = unitNum */
    MF_PARAM = unitNum;

    mf_issue_cmd(MF_CMD_GETVOLINFO);
    if (mf_last_error != 0) {
        return 0;
    }

    /* Layout matches the firmware VolInfo_t; copy into compact struct. */
    info->type        = MF_PARAM;
    info->block_count = (uint16_t)MF_PARAM;             /* low */
    info->block_count |= ((uint16_t)MF_PARAM) << 8;     /* high */
    info->medium      = MF_PARAM;
    info->name_len    = MF_PARAM;

    /* Copy up to 15 chars, pad/terminate. */
    for (i = 0; i < 15; ++i) {
        if (i < info->name_len) {
            info->name[i] = (char)MF_PARAM;
        } else {
            info->name[i] = '\0';
        }
    }
    info->name[15] = '\0';

    return 1;
}

uint8_t __fastcall__ mf_read_block(uint8_t unitNum,
                                   uint32_t block,
                                   void*    buffer)
{
    uint16_t i;
    uint8_t* p = (uint8_t*)buffer;

    if (!p) {
        return 0xFFu;
    }

    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* unit number */
    MF_PARAM = unitNum;

    /* 24-bit block number, little endian */
    MF_PARAM = (uint8_t)(block & 0xFFu);
    MF_PARAM = (uint8_t)((block >> 8) & 0xFFu);
    MF_PARAM = (uint8_t)((block >> 16) & 0xFFu);

    mf_issue_cmd(MF_CMD_READBLOCK);
    if (mf_last_error != 0) {
        return mf_last_error;
    }

    /* 512 bytes from data stream */
    for (i = 0; i < 512u; ++i) {
        p[i] = MF_DATA;
    }
    return 0;
}

uint8_t __fastcall__ mf_write_block(uint8_t unitNum,
                                    uint32_t block,
                                    const void* buffer)
{
    uint16_t i;
    const uint8_t* p = (const uint8_t*)buffer;

    if (!p) {
        return 0xFFu;
    }

    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* unit number */
    MF_PARAM = unitNum;

    /* 24-bit block number, little endian */
    MF_PARAM = (uint8_t)(block & 0xFFu);
    MF_PARAM = (uint8_t)((block >> 8) & 0xFFu);
    MF_PARAM = (uint8_t)((block >> 16) & 0xFFu);

    /* stream 512 bytes into data register */
    for (i = 0; i < 512u; ++i) {
        MF_DATA = p[i];
    }

    /* destructive ops require write-enable key in param stream */
    MF_PARAM = MF_WE_KEY;

    mf_issue_cmd(MF_CMD_WRITEBLOCK);
    return mf_last_error;
}

void __fastcall__ mf_get_firmware_version(char* out12)
{
    uint8_t i;
    if (!out12) {
        return;
    }

    mf_issue_cmd(MF_CMD_GETFIRMWAREVER);
    if (mf_last_error != 0) {
        out12[0] = '\0';
        return;
    }

    for (i = 0; i < 12u; ++i) {
        out12[i] = (char)MF_PARAM;
    }
}

void __fastcall__ mf_get_time_string(char* out8)
{
    uint8_t i;
    if (!out8) {
        return;
    }

    mf_issue_cmd(MF_CMD_GETTIMESTR);
    if (mf_last_error != 0) {
        out8[0] = '\0';
        return;
    }

    for (i = 0; i < 8u; ++i) {
        out8[i] = (char)MF_PARAM;
    }
}

uint8_t __fastcall__ mf_enable_romdisk_last(void)
{
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    MF_PARAM = 0u;                     /* 0 = last unit */
    mf_issue_cmd(MF_CMD_ENABLEROMDISK);
    return mf_last_error;
}

uint8_t __fastcall__ mf_enable_romdisk_first(void)
{
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    MF_PARAM = 1u;                     /* 1 = first unit */
    mf_issue_cmd(MF_CMD_ENABLEROMDISK);
    return mf_last_error;
}

uint8_t __fastcall__ mf_disable_romdisk(void)
{
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    mf_issue_cmd(MF_CMD_DISABLEROMDISK);
    return mf_last_error;
}

uint8_t __fastcall__ mf_test_wifi(void)
{
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* write-enable key in parameter buffer */
    MF_PARAM = MF_WE_KEY;
    mf_issue_cmd(MF_CMD_TESTWIFI);
    if (mf_last_error != 0) {
        return mf_last_error;
    }
    /* result code comes back in first param byte */
    return MF_PARAM;
}

/* --------------------------------------------------------------------
 * FPU API (MBF-level)
 * ------------------------------------------------------------------ */

void __fastcall__ mf_fpu_op(uint8_t cmd,
                            const mf_fpu_args_t* args,
                            mf_fpu_result_t* res)
{
    uint8_t i;

    if (!args || !res) {
        mf_last_error = 0xFFu;
        return;
    }

    /* Reset both pointers. */
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* Send 13 bytes of FAC/ARG into parameter buffer. */
    for (i = 0; i < 13u; ++i) {
        MF_PARAM = args->bytes[i];
    }

    /* Issue FPU command. */
    mf_issue_cmd(cmd);
    if (mf_last_error != 0) {
        return;
    }

    /* Read 1-byte error code + 7-byte MBF result. */
    for (i = 0; i < 8u; ++i) {
        res->bytes[i] = MF_PARAM;
    }
}

void __fastcall__ mf_fadd(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FADD, args, res);
}

void __fastcall__ mf_fmul(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FMUL, args, res);
}

void __fastcall__ mf_fdiv(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FDIV, args, res);
}

void __fastcall__ mf_fsin(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FSIN, args, res);
}

void __fastcall__ mf_fcos(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FCOS, args, res);
}

void __fastcall__ mf_ftan(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FTAN, args, res);
}

void __fastcall__ mf_fatn(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FATN, args, res);
}

void __fastcall__ mf_flog(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FLOG, args, res);
}

void __fastcall__ mf_fexp(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FEXP, args, res);
}

void __fastcall__ mf_fsqr(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FSQR, args, res);
}

void __fastcall__ mf_fout(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FOUT, args, res);
}



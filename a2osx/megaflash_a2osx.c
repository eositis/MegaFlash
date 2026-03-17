/* MegaFlash library for A2osX C
 *
 * Portable K&R-style C implementation of the MegaFlash $C0C0 interface.
 * Avoids compiler-specific extensions so it can be compiled by the
 * C compiler used in the A2osX project.
 *
 * See docs/a2osx-megaflash-lib.md for API reference and examples.
 */

#include "megaflash_a2osx.h"

mf_u8 mf_last_error = 0;

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
static mf_u8 mf_issue_cmd(mf_u8 cmd)
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

mf_u8 mf_failed(void)
{
    return (mf_last_error != 0);
}

/* --------------------------------------------------------------------
 * Core API
 * ------------------------------------------------------------------ */

mf_u8 mf_get_unit_count(void)
{
    mf_issue_cmd(MF_CMD_GETDEVSTATUS);
    if (mf_last_error != 0) {
        return 0xFFu;
    }
    return MF_PARAM;
}

mf_u8 mf_get_volinfo(mf_u8 unitNum, mf_volinfo_t* info)
{
    mf_u8 i;

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

    /* Copy firmware VolInfo struct into mf_volinfo_t. */
    info->type        = MF_PARAM;
    info->block_count = (mf_u16)MF_PARAM;             /* low */
    info->block_count |= ((mf_u16)MF_PARAM) << 8;     /* high */
    info->medium      = MF_PARAM;
    info->name_len    = MF_PARAM;

    for (i = 0; i < 15u; ++i) {
        if (i < info->name_len) {
            info->name[i] = (char)MF_PARAM;
        } else {
            info->name[i] = '\0';
        }
    }
    info->name[15] = '\0';

    return 1;
}

mf_u8 mf_read_block(mf_u8 unitNum, mf_u32 block, void* buffer)
{
    mf_u16 i;
    mf_u8* p = (mf_u8*)buffer;

    if (!p) {
        return 0xFFu;
    }

    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* unit number */
    MF_PARAM = unitNum;

    /* 24-bit block number, little endian */
    MF_PARAM = (mf_u8)(block & 0xFFu);
    MF_PARAM = (mf_u8)((block >> 8) & 0xFFu);
    MF_PARAM = (mf_u8)((block >> 16) & 0xFFu);

    mf_issue_cmd(MF_CMD_READBLOCK);
    if (mf_last_error != 0) {
        return mf_last_error;
    }

    for (i = 0; i < 512u; ++i) {
        p[i] = MF_DATA;
    }
    return 0;
}

mf_u8 mf_write_block(mf_u8 unitNum, mf_u32 block, const void* buffer)
{
    mf_u16 i;
    const mf_u8* p = (const mf_u8*)buffer;

    if (!p) {
        return 0xFFu;
    }

    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* unit number */
    MF_PARAM = unitNum;

    /* 24-bit block number, little endian */
    MF_PARAM = (mf_u8)(block & 0xFFu);
    MF_PARAM = (mf_u8)((block >> 8) & 0xFFu);
    MF_PARAM = (mf_u8)((block >> 16) & 0xFFu);

    /* stream 512 bytes into data register */
    for (i = 0; i < 512u; ++i) {
        MF_DATA = p[i];
    }

    /* destructive ops require write-enable key in param stream */
    MF_PARAM = MF_WE_KEY;

    mf_issue_cmd(MF_CMD_WRITEBLOCK);
    return mf_last_error;
}

void mf_get_firmware_version(char* out12)
{
    mf_u8 i;
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

void mf_get_time_string(char* out8)
{
    mf_u8 i;
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

mf_u8 mf_enable_romdisk_last(void)
{
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    MF_PARAM = 0u;                     /* 0 = last unit */
    mf_issue_cmd(MF_CMD_ENABLEROMDISK);
    return mf_last_error;
}

mf_u8 mf_enable_romdisk_first(void)
{
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    MF_PARAM = 1u;                     /* 1 = first unit */
    mf_issue_cmd(MF_CMD_ENABLEROMDISK);
    return mf_last_error;
}

mf_u8 mf_disable_romdisk(void)
{
    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    mf_issue_cmd(MF_CMD_DISABLEROMDISK);
    return mf_last_error;
}

mf_u8 mf_test_wifi(void)
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

void mf_fpu_op(mf_u8 cmd,
               const mf_fpu_args_t* args,
               mf_fpu_result_t*     res)
{
    mf_u8 i;

    if (!args || !res) {
        mf_last_error = 0xFFu;
        return;
    }

    MF_CMDREG = MF_CMD_RESETBOTHPTRS;
    mf_wait_busy();

    /* Send 13 bytes of FAC/ARG into parameter buffer. */
    for (i = 0; i < 13u; ++i) {
        MF_PARAM = args->bytes[i];
    }

    mf_issue_cmd(cmd);
    if (mf_last_error != 0) {
        return;
    }

    /* Read 1-byte error code + 7-byte MBF result. */
    for (i = 0; i < 8u; ++i) {
        res->bytes[i] = MF_PARAM;
    }
}

void mf_fadd(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FADD, args, res);
}

void mf_fmul(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FMUL, args, res);
}

void mf_fdiv(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FDIV, args, res);
}

void mf_fsin(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FSIN, args, res);
}

void mf_fcos(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FCOS, args, res);
}

void mf_ftan(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FTAN, args, res);
}

void mf_fatn(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FATN, args, res);
}

void mf_flog(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FLOG, args, res);
}

void mf_fexp(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FEXP, args, res);
}

void mf_fsqr(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FSQR, args, res);
}

void mf_fout(const mf_fpu_args_t* args, mf_fpu_result_t* res)
{
    mf_fpu_op(MF_CMD_FOUT, args, res);
}



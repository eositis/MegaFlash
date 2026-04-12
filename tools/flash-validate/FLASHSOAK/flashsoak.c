/*
 * FLASHSOAK — MegaFlash overnight soak (C / cc65, ProDOS)
 *
 * Port of tools/flash-validate/FLASHSOAK.BAS: same command sequence and CSV log.
 * Run from BASIC.SYSTEM: BRUN FLASHSOAK (or -SYSTEM on some hosts).
 *
 * Slot-relative $C080+slot*16 register base (same formula as Applesoft CR = 49280+16*SL).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>

/* ---- MegaFlash command codes (common/defines.h) ---- */
enum {
    CMD_RESET_BOTHPTRS = 0x00,
    CMD_GETDEVSTATUS = 0x11,
    CMD_GETUNITSTATUS = 0x12,
    CMD_READBLOCK = 0x15,
    CMD_FORMATDISK = 0x1D,
    CMD_TFTPRUN = 0x50,
    CMD_TFTPSTATUS = 0x51,
    CMD_GETTIMESTR = 0x19,
};

#define MF_BUSY 0x80u
#define MF_ERR 0x40u
#define MF_ERRMASK 0x1Fu
#define MF_WE 0x71u

#define SLOT_BASE(S) ((uint16_t)(0xC080u + (uint16_t)(S) * 16u))

#define MAX_UNITS 16u
#define TFTP_MAX_TICKS 3600u
#define FILL_ITER 40000u

static volatile uint8_t *g_cmd;
static volatile uint8_t *g_param;
static volatile uint8_t *g_data;

static uint8_t g_slot;
static char g_host[64];
static char g_logpath[80];
static int g_skip_log;
static int g_log_opened;
static uint8_t g_pad40[41];

/* ---- register helpers (slot-relative) ---- */

static void mf_set_slot(uint8_t slot)
{
    uint16_t b = SLOT_BASE(slot);
    g_cmd = (volatile uint8_t *)(void *)b;
    g_param = (volatile uint8_t *)(void *)(b + 1u);
    g_data = (volatile uint8_t *)(void *)(b + 2u);
    g_slot = slot;
}

static void mf_wait_busy(void)
{
    while (*g_cmd & MF_BUSY) { /* spin */ }
}

static uint8_t mf_issue_cmd(uint8_t cmd)
{
    *g_cmd = cmd;
    mf_wait_busy();
    if (*g_cmd & MF_ERR) {
        return (uint8_t)(*g_cmd & MF_ERRMASK);
    }
    return 0;
}

static void mf_reset(void)
{
    mf_issue_cmd(CMD_RESET_BOTHPTRS);
}

static uint8_t mf_get_unit_count(void)
{
    mf_reset();
    if (mf_issue_cmd(CMD_GETDEVSTATUS) != 0) {
        return 0;
    }
    return *g_param;
}

static uint32_t mf_get_block_count(uint8_t unit)
{
    mf_reset();
    *g_param = unit;
    if (mf_issue_cmd(CMD_GETUNITSTATUS) != 0) {
        return 0;
    }
    {
        uint8_t lo = *g_param;
        uint8_t mi = *g_param;
        uint8_t hi = *g_param;
        return (uint32_t)lo + ((uint32_t)mi << 8) + ((uint32_t)hi << 16);
    }
}

static uint8_t mf_read_block(uint8_t unit, uint32_t block, uint8_t *buf512, uint8_t *re_out)
{
    uint16_t i;
    uint8_t ms, re;
    mf_reset();
    *g_param = unit;
    *g_param = (uint8_t)(block & 0xFFu);
    *g_param = (uint8_t)((block >> 8) & 0xFFu);
    *g_param = (uint8_t)((block >> 16) & 0xFFu);
    (void)mf_issue_cmd(CMD_READBLOCK);
    ms = (uint8_t)(*g_cmd & MF_ERRMASK);
    re = *g_param;
    if (ms != 0) {
        re = ms;
    }
    *re_out = re;
    if (re != 0) {
        return 1;
    }
    for (i = 0; i < 512u; ++i) {
        buf512[i] = *g_data;
    }
    return 0;
}

/* Returns SmartPort-style result in *re; MF error nibble in *me. 0 = OK for re on success path. */
static void mf_format_disk(uint8_t unit, uint32_t blocks, const char *volname, uint8_t *re, uint8_t *me)
{
    const char *p;
    mf_reset();
    *g_param = unit;
    *g_param = (uint8_t)(blocks & 0xFFu);
    *g_param = (uint8_t)((blocks >> 8) & 0xFFu);
    *g_param = (uint8_t)((blocks >> 16) & 0xFFu);
    *g_param = MF_WE;
    for (p = volname; *p; ++p) {
        *g_param = (uint8_t)*p;
    }
    *g_param = 0;
    (void)mf_issue_cmd(CMD_FORMATDISK);
    *re = *g_param;
    *me = (uint8_t)(*g_cmd & MF_ERRMASK);
}

static void mf_get_time_str(char out8[8])
{
    uint8_t i;
    mf_reset();
    if (mf_issue_cmd(CMD_GETTIMESTR) != 0) {
        memset(out8, ' ', 8u);
        return;
    }
    for (i = 0; i < 8u; ++i) {
        out8[i] = (char)(*g_param & 0x7Fu);
    }
}

/* ---- TFTP (same layout as FLASHSOAK.BAS / cmdhandler DoTFTPRun) ---- */

static uint8_t mf_tftp_run(uint8_t unit, uint8_t dir_up, const char *host, const char *fn)
{
    const char *p;
    mf_reset();
    for (p = host; *p; ++p) {
        *g_data = (uint8_t)*p;
    }
    *g_data = 0;
    for (p = fn; *p; ++p) {
        *g_data = (uint8_t)*p;
    }
    *g_data = 0;
    *g_param = unit;
    *g_param = dir_up;
    *g_param = 0;
    *g_param = MF_WE;
    return mf_issue_cmd(CMD_TFTPRUN);
}

static void mf_tftp_status_poll(uint8_t *cf, uint8_t *et)
{
    mf_reset();
    *g_param = 0;
    *g_param = 0;
    *g_param = 100;
    mf_issue_cmd(CMD_TFTPSTATUS);
    *cf = *g_param;
    (void)*g_param; /* progress */
    (void)*g_param;
    (void)*g_param;
    *et = *g_param;
}

/* ---- UI ---- */

static void status_line21(const char *msg)
{
    uint8_t i;
    char line[41];
    const char *m = msg;
    if (!m) {
        m = "";
    }
    for (i = 0; i < 40u; ++i) {
        line[i] = (m[i] && m[i] != '\n') ? m[i] : ' ';
    }
    line[40] = '\0';
    gotoxy(0, 20);
    cputs(line);
}

static void banner(void)
{
    clrscr();
    cputs("MEGAFLASH FLASHSOAK (C)\r\n");
}

static uint8_t read_slot(void)
{
    char buf[64];
    char *p;
    long v;
    cputs("SLOT (1-7, default 4): ");
    if (fgets(buf, sizeof buf, stdin) == NULL) {
        return 4;
    }
    p = buf;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r') {
        return 4;
    }
    v = strtol(p, NULL, 10);
    if (v < 1 || v > 7) {
        return 4;
    }
    return (uint8_t)v;
}

static void read_log_prompt(void)
{
    char buf[80];
    char *p;
    g_skip_log = 0;
    g_logpath[0] = '\0';
    cputs("LOG (NONE=OFF, default SOAK.TXT): ");
    if (fgets(buf, sizeof buf, stdin) == NULL) {
        strcpy(g_logpath, "SOAK.TXT");
        return;
    }
    p = buf;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r') {
        strcpy(g_logpath, "SOAK.TXT");
        return;
    }
    if ((p[0] == 'N' || p[0] == 'n') && (p[1] == 'O' || p[1] == 'o') && (p[2] == 'N' || p[2] == 'n') &&
        (p[3] == 'E' || p[3] == 'e')) {
        g_skip_log = 1;
        g_log_opened = 1;
        cputs("LOG FILE OFF\r\n");
        return;
    }
    {
        size_t n = strcspn(p, "\r\n");
        if (n >= sizeof g_logpath) {
            n = sizeof g_logpath - 1u;
        }
        memcpy(g_logpath, p, n);
        g_logpath[n] = '\0';
    }
}

static void read_host(void)
{
    strcpy(g_host, "192.168.0.10");
    cputs("TFTP SERVER (RETURN=192.168.0.10): ");
    {
        char buf[64];
        char *p;
        if (fgets(buf, sizeof buf, stdin) != NULL) {
            p = buf;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (*p && *p != '\n' && *p != '\r') {
                size_t n = strcspn(p, "\r\n");
                if (n >= sizeof g_host) {
                    n = sizeof g_host - 1u;
                }
                memcpy(g_host, p, n);
                g_host[n] = '\0';
            }
        }
    }
}

/* ---- 16-bit checksum (same as BASIC) ---- */

static uint16_t checksum_volume(uint8_t unit, uint32_t nblocks, uint8_t *buf, uint8_t *rd_err)
{
    uint32_t bn;
    uint16_t cs = 0;
    uint16_t j;
    uint8_t re = 0;
    *rd_err = 0;
    for (bn = 0; bn < nblocks; ++bn) {
        if (mf_read_block(unit, bn, buf, &re) != 0) {
            *rd_err = re;
            break;
        }
        for (j = 0; j < 512u; ++j) {
            cs = (uint16_t)(cs + buf[j]);
        }
    }
    return cs;
}

/* ---- CSV log ---- */

static FILE *g_logfp;

static void log_append(int cycle, int unit, const char *ev, const char *rs, int v1, int v2)
{
    char ts[9];
    if (g_skip_log) {
        return;
    }
    if (!g_logfp) {
        return;
    }
    mf_get_time_str(ts);
    ts[8] = '\0';
    fprintf(g_logfp, "%s,%d,%d,%s,%s,%d,%d\r\n", ts, cycle, unit, ev, rs, v1, v2);
    fflush(g_logfp);
}

static void log_open_run_start(int cycle)
{
    char ts[9];
    int is_new = 0;
    FILE *t;
    if (g_skip_log || g_log_opened) {
        return;
    }
    status_line21("OPENING LOG");
    mf_get_time_str(ts);
    ts[8] = '\0';
    t = fopen(g_logpath, "r");
    if (!t) {
        is_new = 1;
    } else {
        fclose(t);
    }
    g_logfp = fopen(g_logpath, "a");
    if (!g_logfp) {
        cputs("CANNOT OPEN LOG FILE\r\n");
        return;
    }
    if (is_new) {
        fprintf(g_logfp, "ts,cycle,unit,event,result,v1,v2\r\n");
    }
    fprintf(g_logfp, "%s,%d,%d,%s,%s,%d,%d\r\n", ts, cycle, 0, "RUN_START", "OK", 0, 0);
    fflush(g_logfp);
    g_log_opened = 1;
}

/* ---- File workload (ProDOS paths /VOLNAME/file) ---- */

static int file_workload(const char *vn, int cy, int u)
{
    char path[96];
    FILE *fp;
    long i;
    sprintf(path, "/%s/A.TXT", vn);
    fp = fopen(path, "w");
    if (!fp) {
        return 1;
    }
    for (i = 1; i <= 120; ++i) {
        fprintf(fp, "A,%d,%d,%ld\r\n", cy, u, i);
    }
    fclose(fp);
    sprintf(path, "/%s/B.TXT", vn);
    fp = fopen(path, "w");
    if (!fp) {
        return 1;
    }
    for (i = 1; i <= 180; ++i) {
        fprintf(fp, "B0,%d,%d,%ld\r\n", cy, u, i);
    }
    fclose(fp);
    sprintf(path, "/%s/C.TXT", vn);
    fp = fopen(path, "w");
    if (!fp) {
        return 1;
    }
    for (i = 1; i <= 120; ++i) {
        fprintf(fp, "C,%d,%d,%ld\r\n", cy, u, i);
    }
    fclose(fp);
    sprintf(path, "/%s/B.TXT", vn);
    fp = fopen(path, "a");
    if (!fp) {
        return 1;
    }
    for (i = 1; i <= 120; ++i) {
        fprintf(fp, "B1,%d,%d,%ld\r\n", cy, u, i);
    }
    fclose(fp);
    sprintf(path, "/%s/C.TXT", vn);
    remove(path);
    sprintf(path, "/%s/FILL.TXT", vn);
    fp = fopen(path, "w");
    if (!fp) {
        return 1;
    }
    status_line21("FILE FILL (LONG)");
    for (i = 1; i <= (long)FILL_ITER; ++i) {
        fprintf(fp, "F,%d,%d,%ld\r\n", cy, u, i);
        if ((i & 0x3FFFL) == 0) {
            fflush(fp);
        }
    }
    fclose(fp);
    return 0;
}

/* ---- TFTP wait loop ---- */

static int tftp_wait_ok(void)
{
    uint16_t tt;
    uint8_t cf, et;
    char wbuf[48];
    for (tt = 1; tt <= TFTP_MAX_TICKS; ++tt) {
        mf_tftp_status_poll(&cf, &et);
        if (cf == 1u) {
            return 0;
        }
        if (cf == 255u) {
            return 100 + (int)et;
        }
        if ((tt % 50u) == 0u) {
            sprintf(wbuf, "TFTP WAIT %u", (unsigned)tt);
            status_line21(wbuf);
        }
        {
            uint16_t z;
            for (z = 0; z < 150u; ++z) {
                /* delay */
            }
        }
    }
    return 98;
}

static int tftp_xfer(uint8_t unit, uint8_t up, const char *fn)
{
    uint8_t e;
    e = mf_tftp_run(unit, up, g_host, fn);
    if (e != 0) {
        return 200 + (int)e;
    }
    return tftp_wait_ok();
}

/* ---- One cycle ---- */

static uint8_t s_blk[512];

static int one_cycle(unsigned cyc)
{
    uint8_t uc, u;
    uint32_t bc;
    char vn[16];
    char fn[32];
    char us[8];
    int ap = 1;
    uint16_t c1, c2;
    int te;
    uint8_t re, me;
    uint8_t rd_err;

    if (!g_skip_log && !g_log_opened) {
        log_open_run_start((int)cyc);
    }

    mf_reset();
    uc = mf_get_unit_count();
    if (uc == 0 || uc == 0xFFu) {
        cputs("NO UNITS (CHECK MEGAFLASH)\r\n");
        return 1;
    }
    if (uc > MAX_UNITS) {
        uc = MAX_UNITS;
    }

    log_append((int)cyc, 0, "CYCLE_START", "OK", (int)uc, 0);
    {
        char mbuf[48];
        sprintf(mbuf, "CY%u START %u UNITS", (unsigned)cyc, (unsigned)uc);
        status_line21(mbuf);
    }

    for (u = 1; u <= uc; ++u) {
        bc = mf_get_block_count(u);
        sprintf(us, "%u", (unsigned)u);
        sprintf(vn, "VAL%s", us);
        sprintf(fn, "validation%s.po", us);

        if (bc < 32u) {
            log_append((int)cyc, (int)u, "UNIT_SKIP", "SMALL", (int)bc, 0);
            continue;
        }

        log_append((int)cyc, (int)u, "UNIT_START", "OK", (int)bc, 0);
        {
            char mbuf[48];
            sprintf(mbuf, "CY%u U%s %luBLK", (unsigned)cyc, us, (unsigned long)bc);
            status_line21(mbuf);
        }

        mf_format_disk(u, bc, vn, &re, &me);
        if (re != 0) {
            ap = 0;
            log_append((int)cyc, (int)u, "FORMAT_A", "FAIL", (int)re, (int)me);
            continue;
        }
        log_append((int)cyc, (int)u, "FORMAT_A", "OK", 0, 0);

        if (file_workload(vn, (int)cyc, (int)u) != 0) {
            ap = 0;
            log_append((int)cyc, (int)u, "FILES", "FAIL", 1, 0);
            continue;
        }
        log_append((int)cyc, (int)u, "FILES", "OK", 0, 0);

        rd_err = 0;
        c1 = checksum_volume(u, bc, s_blk, &rd_err);
        if (rd_err != 0) {
            ap = 0;
            log_append((int)cyc, (int)u, "CHK_A", "FAIL", (int)rd_err, 0);
            continue;
        }
        log_append((int)cyc, (int)u, "CHK_A", "OK", (int)c1, 0);

        status_line21("TFTP UP");
        te = tftp_xfer(u, 1, fn);
        if (te != 0) {
            ap = 0;
            log_append((int)cyc, (int)u, "TFTP_UP", "FAIL", te, 0);
            continue;
        }
        log_append((int)cyc, (int)u, "TFTP_UP", "OK", 0, 0);

        mf_format_disk(u, bc, vn, &re, &me);
        if (re != 0) {
            ap = 0;
            log_append((int)cyc, (int)u, "FORMAT_B", "FAIL", (int)re, (int)me);
            continue;
        }
        log_append((int)cyc, (int)u, "FORMAT_B", "OK", 0, 0);

        status_line21("TFTP DN");
        te = tftp_xfer(u, 0, fn);
        if (te != 0) {
            ap = 0;
            log_append((int)cyc, (int)u, "TFTP_DN", "FAIL", te, 0);
            continue;
        }
        log_append((int)cyc, (int)u, "TFTP_DN", "OK", 0, 0);

        rd_err = 0;
        c2 = checksum_volume(u, bc, s_blk, &rd_err);
        if (rd_err != 0) {
            ap = 0;
            log_append((int)cyc, (int)u, "CHK_B", "FAIL", (int)rd_err, 0);
            continue;
        }
        if (c2 != c1) {
            ap = 0;
            log_append((int)cyc, (int)u, "CHK_B", "MISMATCH", (int)c1, (int)c2);
        } else {
            log_append((int)cyc, (int)u, "CHK_B", "OK", (int)c2, 0);
        }
    }

    if (!ap) {
        log_append((int)cyc, 0, "CYCLE_END", "FAIL", 0, 0);
        status_line21("CYCLE END FAIL");
        return 0;
    }
    log_append((int)cyc, 0, "CYCLE_END", "PASS", 0, 0);
    status_line21("REFORMAT ALL UNITS");
    for (u = 1; u <= uc; ++u) {
        bc = mf_get_block_count(u);
        sprintf(us, "%u", (unsigned)u);
        sprintf(vn, "VAL%s", us);
        mf_format_disk(u, bc, vn, &re, &me);
        log_append((int)cyc, (int)u, "REFORMAT", re == 0 ? "OK" : "FAIL", (int)re, (int)me);
    }
    status_line21("CYCLE END PASS");
    return 0;
}

int main(void)
{
    unsigned cyc = 1;
    uint8_t slot;

    {
        uint8_t i;
        for (i = 0; i < 40u; ++i) {
            g_pad40[i] = ' ';
        }
        g_pad40[40] = '\0';
    }

    banner();
    slot = read_slot();
    mf_set_slot(slot);
    {
        char sbuf[48];
        sprintf(sbuf, "SLOT %u BASE $%04X", (unsigned)slot, (unsigned)SLOT_BASE(slot));
        cputs(sbuf);
        cputs("\r\n");
    }

    read_log_prompt();
    read_host();

    cputs("PROGRESS = LINE 21. CTRL-RESET=STOP\r\n");
    gotoxy(0, 19);
    cputs("----------------------------------------\r\n");

    for (;;) {
        one_cycle(cyc);
        ++cyc;
    }
    return 0;
}

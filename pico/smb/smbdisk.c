#include "smbdisk.h"
#include "userconfig.h"
#include "defines.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include <string.h>
#include <stdio.h>

#ifndef MEGAFLASH_SMB
#define MEGAFLASH_SMB 1
#endif

#if MEGAFLASH_SMB

#include "smb_prodos.h"
#include "smb_client.h"

#define SMB_IO_TIMEOUT_MS 8000

typedef struct {
  volatile uint32_t op; /* 0 idle, 1 read, 2 write */
  volatile uint32_t block;
  uint8_t *buf;
  const uint8_t *src;
  volatile rwerror_t result;
} SmbIoReq;

static SmbIoReq io_;
static bool servicing_;

void SmbDisk_Init(void) {
  memset(&io_, 0, sizeof(io_));
  SmbClient_Init();
}

void SmbDisk_OnConfigChanged(void) {
  SmbProdos_Invalidate();
  SmbClient_OnConfigChanged();
}

uint32_t SmbDisk_GetUnitCount(void) {
  return SmbClient_WantsUnit() ? 1u : 0u;
}

bool SmbDisk_Online(void) {
  return SmbClient_IsReady();
}

uint32_t SmbDisk_GetBlockCount(void) {
  return 0xFFFFu;
}

void SmbDisk_GetDIB(uint8_t *destBuffer) {
  struct dib_t *dib = (struct dib_t *)destBuffer;
  memset(dib, 0, sizeof(*dib));
  uint8_t online = SmbDisk_Online() ? 0x10 : 0;
  dib->devicestatus = (uint8_t)(0b11101000 | online); /* block, write, read, online?, no format */
  if (!SmbDisk_Online()) dib->devicestatus = 0b11100000;
  uint32_t blocks = SmbDisk_GetBlockCount();
  dib->blocksize_l = (uint8_t)blocks;
  dib->blocksize_m = (uint8_t)(blocks >> 8);
  dib->blocksize_h = (uint8_t)(blocks >> 16);
  dib->idstrlen = 15;
  SmbProdos_FillDibId(dib->idstr);
  dib->devicetype = 0x02; /* ProFile hard disk */
  dib->subtype = 0x20;
  dib->fmversion_l = (uint8_t)FIRMWAREVER;
  dib->fmversion_h = (uint8_t)(FIRMWAREVER >> 8);
}

static rwerror_t WaitIo(uint32_t op, uint32_t block, uint8_t *rw, const uint8_t *src) {
  io_.block = block;
  io_.buf = rw;
  io_.src = src;
  io_.result = SP_IOERR;
  __dmb();
  io_.op = op;
  absolute_time_t deadline = make_timeout_time_ms(SMB_IO_TIMEOUT_MS);
  while (io_.op != 0 && !time_reached(deadline)) {
    tight_loop_contents();
  }
  if (io_.op != 0) {
    io_.op = 0;
    return SP_IOERR;
  }
  return io_.result;
}

rwerror_t SmbDisk_ReadBlock(uint32_t blockNum, uint8_t *destBuffer) {
  if (!SmbClient_WantsUnit()) return SP_NODRVERR;
  return WaitIo(1, blockNum, destBuffer, NULL);
}

rwerror_t SmbDisk_WriteBlock(uint32_t blockNum, const uint8_t *srcBuffer) {
  if (!SmbClient_WantsUnit()) return SP_NODRVERR;
  return WaitIo(2, blockNum, NULL, srcBuffer);
}

void SmbDisk_GetStatusText(char *dest, uint32_t destlen) {
  if (!dest || destlen == 0) return;
  snprintf(dest, destlen, "%s", SmbClient_StatusText());
}

void SmbDisk_ServiceCore0(void) {
  if (servicing_) return;
  uint32_t op = io_.op;
  if (op == 0) return;
  servicing_ = true;
  rwerror_t rc = SP_IOERR;
  if (op == 1) rc = SmbProdos_ReadBlock(io_.block, io_.buf);
  else if (op == 2) rc = SmbProdos_WriteBlock(io_.block, io_.src);
  io_.result = rc;
  __dmb();
  io_.op = 0;
  servicing_ = false;
}

#else /* MEGAFLASH_SMB == 0: omit client BSS on RP2040 */

void SmbDisk_Init(void) {}
void SmbDisk_OnConfigChanged(void) {}
uint32_t SmbDisk_GetUnitCount(void) { return 0; }
bool SmbDisk_Online(void) { return false; }
uint32_t SmbDisk_GetBlockCount(void) { return 0; }
void SmbDisk_GetDIB(uint8_t *destBuffer) { (void)destBuffer; }
rwerror_t SmbDisk_ReadBlock(uint32_t blockNum, uint8_t *destBuffer) {
  (void)blockNum;
  (void)destBuffer;
  return SP_NODRVERR;
}
rwerror_t SmbDisk_WriteBlock(uint32_t blockNum, const uint8_t *srcBuffer) {
  (void)blockNum;
  (void)srcBuffer;
  return SP_NODRVERR;
}
void SmbDisk_GetStatusText(char *dest, uint32_t destlen) {
  if (!dest || destlen == 0) return;
  snprintf(dest, destlen, "Failed: SMB compiled out (Pico W RAM)");
}
void SmbDisk_ServiceCore0(void) {}

#endif

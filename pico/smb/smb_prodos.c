#include "smb_prodos.h"
#include "smb_client.h"
#include "smbdisk.h"
#include "userconfig.h"
#include "formatter.h"
#include "defines.h"
#include "misc.h"
#include "pico/time.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#define VOL_BLOCKS     0xFFFFu
#define MAX_FILES      SMB_MAX_DIR_ENTS
#define KEY_BASE       22
#define DATA_BASE      64
#define SUB_BASE       1024u
#define BLOCKS_PER_FILE 2048u
#define DIR_CACHE_MS   3000u
#define ROOT_MAX       50

#define FT_TXT  0x04
#define FT_BIN  0x06
#define FT_DIR  0x0F
#define FT_BAS  0xFC
#define FT_SYS  0xFF

typedef struct {
  char prodos[16];
  char smb[SMB_NAME_MAX];
  uint8_t type;
  uint16_t auxtype;
  uint32_t eof;
  bool is_dir;
  bool in_use;
} FileMap;

static FileMap files_[MAX_FILES];
static int file_count_;
static FileMap root_files_[ROOT_MAX];
static int root_count_;
static uint8_t sub_blocks_[4][512];
static int sub_idx_ = -1;
static bool catalog_valid_;
static absolute_time_t catalog_expiry_;
static char volname_[16];
static uint8_t volname_len_;
static uint8_t dir_blocks_[4][512];

static void SanitizeProdos(const char *src, char *dst15) {
  char tmp[16];
  memset(tmp, 0, sizeof(tmp));
  int j = 0;
  for (int i = 0; src[i] && j < 15; i++) {
    unsigned char raw = (unsigned char)src[i];
    int lower = islower(raw);
    char c = (char)toupper(raw);
    if (j == 0 && !isalpha((unsigned char)c)) continue;
    if (isalnum((unsigned char)c) || c == '.') {
      /* GS/OS / Apple II Desktop: high bit marks lowercase */
      tmp[j++] = (char)(lower ? ((unsigned char)c | 0x80u) : (unsigned char)c);
    }
  }
  if (j == 0 || !isalpha((unsigned char)(tmp[0] & 0x7f))) {
    strcpy(tmp, "FILE");
    j = 4;
  }
  memcpy(dst15, tmp, 15);
  dst15[15] = 0;
}

static void UniqueName(char *name15) {
  char base[16];
  memcpy(base, name15, 16);
  for (int n = 0; n < 99; n++) {
    bool clash = false;
    for (int i = 0; i < file_count_; i++) {
      if (files_[i].in_use && strncmp(files_[i].prodos, name15, 15) == 0) {
        clash = true;
        break;
      }
    }
    if (!clash) return;
    snprintf(name15, 16, "%.12s.%d", base, n + 1);
    for (int k = 0; name15[k]; k++) name15[k] = (char)toupper((unsigned char)name15[k]);
  }
}

static uint8_t TypeFromName(const char *smb, uint16_t *aux) {
  *aux = 0;
  const char *dot = strrchr(smb, '.');
  if (!dot) return FT_BIN;
  if (stricmp(dot, ".TXT") == 0 || stricmp(dot, ".TEXT") == 0) return FT_TXT;
  if (stricmp(dot, ".SYS") == 0) return FT_SYS;
  if (stricmp(dot, ".BAS") == 0) return FT_BAS;
  if (stricmp(dot, ".BIN") == 0) return FT_BIN;
  return FT_BIN;
}

static void MapToSmbName(const char *prodos, uint8_t type, char *smb_out) {
  char n[16];
  memcpy(n, prodos, 15);
  n[15] = 0;
  /* trim */
  for (int i = 14; i >= 0 && (n[i] == ' ' || n[i] == 0); i--) n[i] = 0;
  if (type == FT_TXT && !strchr(n, '.')) snprintf(smb_out, SMB_NAME_MAX, "%s.TXT", n);
  else strncpy(smb_out, n, SMB_NAME_MAX - 1);
}

static uint16_t KeyBlock(int idx) { return (uint16_t)(KEY_BASE + idx); }
static uint32_t DataBlock(int idx, uint32_t file_blk) {
  return DATA_BASE + (uint32_t)idx * BLOCKS_PER_FILE + file_blk;
}

static int FileFromDataBlock(uint32_t block, uint32_t *file_blk) {
  if (block < DATA_BASE) return -1;
  uint32_t rel = block - DATA_BASE;
  int idx = (int)(rel / BLOCKS_PER_FILE);
  if (idx < 0 || idx >= MAX_FILES) return -1;
  *file_blk = rel % BLOCKS_PER_FILE;
  return idx;
}

static void Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put24(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16);
}

static void BuildDirEntry(uint8_t *e, const FileMap *f, int idx) {
  memset(e, 0, 39);
  uint8_t nlen = 0;
  while (nlen < 15 && f->prodos[nlen]) nlen++;
  uint8_t st = f->is_dir ? 0x0D : 0x02; /* sapling or subdirectory */
  if (!f->is_dir && f->eof <= 512) st = 0x01;
  e[0] = (uint8_t)((st << 4) | (nlen & 0x0f));
  memcpy(e + 1, f->prodos, nlen);
  e[16] = f->is_dir ? FT_DIR : f->type;
  uint16_t key = f->is_dir ? (uint16_t)(SUB_BASE + (uint32_t)idx * 4u) : KeyBlock(idx);
  if (!f->is_dir && st == 0x01) key = (uint16_t)DataBlock(idx, 0);
  Put16(e + 17, key);
  uint16_t used = (uint16_t)((f->eof + 511) / 512);
  if (used == 0) used = 1;
  Put16(e + 19, used);
  Put24(e + 20, f->eof > 0xFFFFFFu ? 0xFFFFFFu : f->eof);
  e[29] = 0xC3; /* access */
  Put16(e + 30, f->auxtype);
  Put16(e + 36, 2); /* header pointer block 2 */
}

static void RebuildDirBlocks(void) {
  memset(dir_blocks_, 0, sizeof(dir_blocks_));
  /* prev/next */
  Put16(dir_blocks_[0] + 0, 0);
  Put16(dir_blocks_[0] + 2, 3);
  Put16(dir_blocks_[1] + 0, 2);
  Put16(dir_blocks_[1] + 2, 4);
  Put16(dir_blocks_[2] + 0, 3);
  Put16(dir_blocks_[2] + 2, 5);
  Put16(dir_blocks_[3] + 0, 4);
  Put16(dir_blocks_[3] + 2, 0);

  uint8_t *vdh = dir_blocks_[0] + 4;
  vdh[0] = (uint8_t)(0xF0 | (volname_len_ & 0x0f));
  memcpy(vdh + 1, volname_, 15);
  dir_blocks_[0][0x22] = 0xC3;
  dir_blocks_[0][0x23] = 0x27;
  dir_blocks_[0][0x24] = 0x0D;
  Put16(dir_blocks_[0] + 0x25, (uint16_t)file_count_);
  Put16(dir_blocks_[0] + 0x27, 6);
  Put16(dir_blocks_[0] + 0x29, VOL_BLOCKS);

  int slot = 0;
  for (int i = 0; i < file_count_; i++) {
    if (!files_[i].in_use) continue;
    int blk = (slot + 1) / 13; /* first entry of blk0 is VDH */
    int ent = (slot + 1) % 13;
    if (blk > 3) break;
    BuildDirEntry(dir_blocks_[blk] + 4 + ent * 39, &files_[i], i);
    slot++;
  }
}

static bool RefreshCatalog(void) {
  if (catalog_valid_ && !time_reached(catalog_expiry_)) return true;
  if (!SmbClient_IsReady()) return false;

  memset(volname_, 0, sizeof(volname_));
  char share[16];
  strncpy(share, GetSmbShare(), 15);
  volname_len_ = (uint8_t)SanitizeVolumeName(share);
  memcpy(volname_, share, 15);

  smb_dirent_t ents[ROOT_MAX];
  int n = 0;
  if (SmbClient_ListDir("", ents, ROOT_MAX, &n) != 0) {
    file_count_ = 0;
    RebuildDirBlocks();
    return false;
  }
  memset(files_, 0, sizeof(files_));
  file_count_ = 0;
  for (int i = 0; i < n && file_count_ < MAX_FILES; i++) {
    FileMap *f = &files_[file_count_];
    memset(f, 0, sizeof(*f));
    SanitizeProdos(ents[i].smb_name, f->prodos);
    UniqueName(f->prodos);
    strncpy(f->smb, ents[i].smb_name, SMB_NAME_MAX - 1);
    f->is_dir = ents[i].is_dir;
    f->eof = ents[i].size > 0x100000u ? 0x100000u : (uint32_t)ents[i].size; /* cap 1 MiB */
    if (f->is_dir) {
      f->type = FT_DIR;
    } else {
      f->type = TypeFromName(f->smb, &f->auxtype);
    }
    f->in_use = true;
    file_count_++;
  }
  root_count_ = file_count_ < ROOT_MAX ? file_count_ : ROOT_MAX;
  memcpy(root_files_, files_, sizeof(FileMap) * (size_t)root_count_);
  RebuildDirBlocks();
  catalog_valid_ = true;
  catalog_expiry_ = make_timeout_time_ms(DIR_CACHE_MS);
  return true;
}

static void BootBlock(uint8_t *dest) {
  memset(dest, 0, 512);
  dest[0] = 0x01; /* not an empty volume */
}

static void BitmapBlock(uint32_t bmpIndex, uint8_t *dest) {
  /* Used: 0 .. DATA_BASE-1 always; plus each file's data blocks. Rest free. */
  memset(dest, 0xFF, 512); /* 1 = free in ProDOS bitmap */
  uint32_t startbit = bmpIndex * 4096u; /* 512*8 */
  for (uint32_t b = 0; b < 4096; b++) {
    uint32_t blk = startbit + b;
    bool used = false;
    if (blk < DATA_BASE) used = true;
    int idx;
    uint32_t fb;
    idx = FileFromDataBlock(blk, &fb);
    if (idx >= 0 && idx < file_count_ && files_[idx].in_use) {
      uint32_t need = (files_[idx].eof + 511) / 512;
      if (fb < need) used = true;
    }
    if (blk >= KEY_BASE && blk < DATA_BASE) used = true;
    if (used) {
      dest[b / 8] &= (uint8_t)~(0x80u >> (b % 8));
    }
  }
}

static void IndexBlock(int idx, uint8_t *dest) {
  memset(dest, 0, 512);
  if (idx < 0 || idx >= file_count_ || !files_[idx].in_use) return;
  uint32_t need = (files_[idx].eof + 511) / 512;
  if (need > 256) need = 256;
  for (uint32_t i = 0; i < need; i++) {
    uint16_t db = (uint16_t)DataBlock(idx, i);
    dest[i] = (uint8_t)db;
    dest[256 + i] = (uint8_t)(db >> 8);
  }
}

void SmbProdos_Invalidate(void) {
  catalog_valid_ = false;
  file_count_ = 0;
}

void SmbProdos_FillDibId(char *id16) {
  memset(id16, ' ', 16);
  const char *s = GetSmbShare();
  char n[16];
  memset(n, 0, sizeof(n));
  strncpy(n, s, 15);
  SanitizeVolumeName(n);
  size_t L = strlen(n);
  if (L > 16) L = 16;
  memcpy(id16, n, L);
}

static bool RefreshSubdir(int idx) {
  static smb_dirent_t ents[MAX_FILES];
  int n = 0, i, slot;
  if (idx < 0 || idx >= root_count_ || !root_files_[idx].is_dir)
    return false;
  if (sub_idx_ == idx && catalog_valid_)
    return true;
  if (!SmbClient_IsReady())
    return false;
  if (SmbClient_ListDir(root_files_[idx].smb, ents, MAX_FILES, &n) != 0)
    n = 0;
  memset(sub_blocks_, 0, sizeof(sub_blocks_));
  Put16(sub_blocks_[0] + 0, 0);
  Put16(sub_blocks_[0] + 2, 3);
  Put16(sub_blocks_[1] + 0, 2);
  Put16(sub_blocks_[1] + 2, 4);
  Put16(sub_blocks_[2] + 0, 3);
  Put16(sub_blocks_[2] + 2, 5);
  Put16(sub_blocks_[3] + 0, 4);
  Put16(sub_blocks_[3] + 2, 0);
  {
    uint8_t *dh = sub_blocks_[0] + 4;
    uint8_t nlen = 0;
    while (nlen < 15 && root_files_[idx].prodos[nlen]) nlen++;
    dh[0] = (uint8_t)(0xE0 | (nlen & 0x0f)); /* subdirectory header */
    memcpy(dh + 1, root_files_[idx].prodos, nlen);
    sub_blocks_[0][0x22] = 0xC3;
    sub_blocks_[0][0x23] = 0x27;
    sub_blocks_[0][0x24] = 0x0D;
    Put16(sub_blocks_[0] + 0x25, (uint16_t)n);
    Put16(sub_blocks_[0] + 0x27, 6);
    Put16(sub_blocks_[0] + 0x29, VOL_BLOCKS);
    Put16(sub_blocks_[0] + 0x1F, 2); /* parent dir block */
  }
  slot = 0;
  for (i = 0; i < n && i < MAX_FILES; i++) {
    FileMap tmp;
    int blk, ent;
    memset(&tmp, 0, sizeof(tmp));
    SanitizeProdos(ents[i].smb_name, tmp.prodos);
    strncpy(tmp.smb, ents[i].smb_name, SMB_NAME_MAX - 1);
    tmp.is_dir = ents[i].is_dir;
    tmp.eof = ents[i].size > 0x100000u ? 0x100000u : (uint32_t)ents[i].size;
    tmp.type = tmp.is_dir ? FT_DIR : TypeFromName(tmp.smb, &tmp.auxtype);
    tmp.in_use = true;
    blk = (slot + 1) / 13;
    ent = (slot + 1) % 13;
    if (blk > 3) break;
    BuildDirEntry(sub_blocks_[blk] + 4 + ent * 39, &tmp, i);
    slot++;
  }
  sub_idx_ = idx;
  return true;
}

rwerror_t SmbProdos_ReadBlock(uint32_t blockNum, uint8_t *dest) {
  if (!SmbClient_IsReady()) return SP_IOERR;
  RefreshCatalog();
  if (blockNum <= 1) {
    BootBlock(dest);
    return SP_NOERR;
  }
  if (blockNum >= 2 && blockNum <= 5) {
    memcpy(dest, dir_blocks_[blockNum - 2], 512);
    return SP_NOERR;
  }
  if (blockNum >= 6 && blockNum <= 21) {
    BitmapBlock(blockNum - 6, dest);
    return SP_NOERR;
  }
  if (blockNum >= SUB_BASE && blockNum < SUB_BASE + (uint32_t)ROOT_MAX * 4u) {
    int idx = (int)((blockNum - SUB_BASE) / 4u);
    int loc = (int)((blockNum - SUB_BASE) % 4u);
    if (!RefreshSubdir(idx)) return SP_IOERR;
    memcpy(dest, sub_blocks_[loc], 512);
    return SP_NOERR;
  }
  if (blockNum >= KEY_BASE && blockNum < DATA_BASE) {
    IndexBlock((int)(blockNum - KEY_BASE), dest);
    return SP_NOERR;
  }
  uint32_t fblk = 0;
  int idx = FileFromDataBlock(blockNum, &fblk);
  if (idx < 0 || !files_[idx].in_use || files_[idx].is_dir) {
    memset(dest, 0, 512);
    return SP_NOERR;
  }
  uint32_t got = 0;
  int rc = SmbClient_Read(files_[idx].smb, (uint64_t)fblk * 512ull, dest, 512, &got);
  if (rc != 0) return SP_IOERR;
  if (got < 512) memset(dest + got, 0, 512 - got);
  return SP_NOERR;
}

static void SyncDirWrite(const uint8_t *src, uint32_t which) {
  memcpy(dir_blocks_[which], src, 512);
  /* Scan entries vs files_ for creates */
  for (int blk = 0; blk < 4; blk++) {
    for (int ent = 0; ent < 13; ent++) {
      if (blk == 0 && ent == 0) continue;
      const uint8_t *e = dir_blocks_[blk] + 4 + ent * 39;
      uint8_t nl = e[0] & 0x0f;
      uint8_t st = e[0] >> 4;
      if (nl == 0 || st == 0) continue;
      char pn[16];
      memset(pn, 0, sizeof(pn));
      memcpy(pn, e + 1, nl);
      bool found = false;
      for (int i = 0; i < file_count_; i++) {
        if (files_[i].in_use && strncmp(files_[i].prodos, pn, 15) == 0) {
          found = true;
          files_[i].eof = (uint32_t)e[20] | ((uint32_t)e[21] << 8) | ((uint32_t)e[22] << 16);
          files_[i].type = e[16];
          files_[i].auxtype = (uint16_t)(e[30] | (e[31] << 8));
          break;
        }
      }
      if (!found && file_count_ < MAX_FILES && SmbClient_IsReady()) {
        FileMap *f = &files_[file_count_];
        memset(f, 0, sizeof(*f));
        memcpy(f->prodos, pn, 15);
        f->type = e[16];
        f->is_dir = (e[16] == FT_DIR) || ((e[0] >> 4) == 0x0D);
        f->eof = (uint32_t)e[20] | ((uint32_t)e[21] << 8) | ((uint32_t)e[22] << 16);
        f->auxtype = (uint16_t)(e[30] | (e[31] << 8));
        MapToSmbName(f->prodos, f->type, f->smb);
        f->in_use = true;
        SmbClient_Create(f->smb, f->is_dir);
        file_count_++;
      }
    }
  }
}

rwerror_t SmbProdos_WriteBlock(uint32_t blockNum, const uint8_t *src) {
  if (!SmbClient_IsReady()) return SP_IOERR;
  RefreshCatalog();
  if (blockNum <= 1) return SP_NOERR;
  if (blockNum >= 2 && blockNum <= 5) {
    SyncDirWrite(src, blockNum - 2);
    catalog_valid_ = true;
    return SP_NOERR;
  }
  if (blockNum >= 6 && blockNum <= 21) return SP_NOERR; /* bitmap: ignore */
  if (blockNum >= KEY_BASE && blockNum < DATA_BASE) return SP_NOERR;
  uint32_t fblk = 0;
  int idx = FileFromDataBlock(blockNum, &fblk);
  if (idx < 0 || !files_[idx].in_use || files_[idx].is_dir) return SP_NOWRITEERR;
  if (SmbClient_Write(files_[idx].smb, (uint64_t)fblk * 512ull, src, 512) != 0) return SP_IOERR;
  uint32_t end = (fblk + 1) * 512;
  if (end > files_[idx].eof) files_[idx].eof = end;
  return SP_NOERR;
}

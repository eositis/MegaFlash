#ifndef MEGAFLASH_SMB_CLIENT_H
#define MEGAFLASH_SMB_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMB_MAX_DIR_ENTS 100
#define SMB_NAME_MAX 80

typedef struct {
  char smb_name[SMB_NAME_MAX];
  uint64_t size;
  uint32_t attr;
  bool is_dir;
} smb_dirent_t;

void SmbClient_Init(void);
void SmbClient_OnConfigChanged(void);
void SmbClient_Pump(void);
bool SmbClient_IsReady(void);
bool SmbClient_WantsUnit(void);
const char *SmbClient_StatusText(void);

/* Core 0: drive one filesystem op to completion via the session state machine.
 * Returns 0 on success. Call only from Core 0 OnPump / IO handler. */
int SmbClient_ListDir(const char *path, smb_dirent_t *ents, int max_ents, int *count_out);
int SmbClient_Read(const char *path, uint64_t offset, uint8_t *buf, uint32_t len, uint32_t *got);
int SmbClient_Write(const char *path, uint64_t offset, const uint8_t *buf, uint32_t len);
int SmbClient_Create(const char *path, bool directory);
int SmbClient_Unlink(const char *path);

#ifdef __cplusplus
}
#endif

#endif

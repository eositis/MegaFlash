#ifndef MEGAFLASH_SMB_CRYPTO_H
#define MEGAFLASH_SMB_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void smb_md4(const uint8_t *data, size_t len, uint8_t out[16]);
void smb_md5(const uint8_t *data, size_t len, uint8_t out[16]);
void smb_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void smb_hmac_md5(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t out[16]);
void smb_hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t out[32]);
void smb_aes128_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t out[16]);
void smb3_kdf_signkey(const uint8_t *session_key, size_t session_key_len, uint8_t out[16]);
void smb_ascii_to_utf16le(const char *ascii, uint8_t *out, size_t *out_len);
void smb_ascii_to_utf16le_upper(const char *ascii, uint8_t *out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif

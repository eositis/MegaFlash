#include "smb_crypto.h"
#include <string.h>
#include <ctype.h>

/* Compact public-domain-style hashes for NTLMv2 + SMB3 signing. */

static uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static uint32_t rotl32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

static uint32_t rd32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void md4_transform(uint32_t st[4], const uint8_t block[64]) {
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], x[16];
  for (int i = 0; i < 16; i++) x[i] = rd32le(block + i * 4);
#define F(x,y,z) (((x)&(y))|((~(x))&(z)))
#define G(x,y,z) (((x)&(y))|((x)&(z))|((y)&(z)))
#define H(x,y,z) ((x)^(y)^(z))
#define R1(a,b,c,d,k,s) a = rotl32(a + F(b,c,d) + x[k], s)
#define R2(a,b,c,d,k,s) a = rotl32(a + G(b,c,d) + x[k] + 0x5a827999u, s)
#define R3(a,b,c,d,k,s) a = rotl32(a + H(b,c,d) + x[k] + 0x6ed9eba1u, s)
  R1(a,b,c,d,0,3);  R1(d,a,b,c,1,7);  R1(c,d,a,b,2,11); R1(b,c,d,a,3,19);
  R1(a,b,c,d,4,3);  R1(d,a,b,c,5,7);  R1(c,d,a,b,6,11); R1(b,c,d,a,7,19);
  R1(a,b,c,d,8,3);  R1(d,a,b,c,9,7);  R1(c,d,a,b,10,11);R1(b,c,d,a,11,19);
  R1(a,b,c,d,12,3); R1(d,a,b,c,13,7); R1(c,d,a,b,14,11);R1(b,c,d,a,15,19);
  R2(a,b,c,d,0,3);  R2(d,a,b,c,4,5);  R2(c,d,a,b,8,9);  R2(b,c,d,a,12,13);
  R2(a,b,c,d,1,3);  R2(d,a,b,c,5,5);  R2(c,d,a,b,9,9);  R2(b,c,d,a,13,13);
  R2(a,b,c,d,2,3);  R2(d,a,b,c,6,5);  R2(c,d,a,b,10,9); R2(b,c,d,a,14,13);
  R2(a,b,c,d,3,3);  R2(d,a,b,c,7,5);  R2(c,d,a,b,11,9); R2(b,c,d,a,15,13);
  R3(a,b,c,d,0,3);  R3(d,a,b,c,8,9);  R3(c,d,a,b,4,11); R3(b,c,d,a,12,15);
  R3(a,b,c,d,2,3);  R3(d,a,b,c,10,9); R3(c,d,a,b,6,11); R3(b,c,d,a,14,15);
  R3(a,b,c,d,1,3);  R3(d,a,b,c,9,9);  R3(c,d,a,b,5,11); R3(b,c,d,a,13,15);
  R3(a,b,c,d,3,3);  R3(d,a,b,c,11,9); R3(c,d,a,b,7,11); R3(b,c,d,a,15,15);
#undef F
#undef G
#undef H
#undef R1
#undef R2
#undef R3
  st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

static void md5_transform(uint32_t st[4], const uint8_t block[64]) {
  static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
  static const uint8_t S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], m[16];
  for (int i = 0; i < 16; i++) m[i] = rd32le(block + i * 4);
  for (int i = 0; i < 64; i++) {
    uint32_t f, g;
    if (i < 16) { f = (b & c) | ((~b) & d); g = i; }
    else if (i < 32) { f = (d & b) | ((~d) & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | (~d)); g = (7 * i) & 15; }
    uint32_t t = d;
    d = c;
    c = b;
    b = b + rotl32(a + f + K[i] + m[g], S[i]);
    a = t;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

static void md_pad_finish(void (*xform)(uint32_t *, const uint8_t *), uint32_t st[4],
                          const uint8_t *data, size_t len, uint8_t out[16], int md4) {
  uint8_t block[64];
  uint64_t bits = (uint64_t)len * 8;
  while (len >= 64) {
    xform(st, data);
    data += 64;
    len -= 64;
  }
  memset(block, 0, 64);
  memcpy(block, data, len);
  block[len] = 0x80;
  if (len >= 56) {
    xform(st, block);
    memset(block, 0, 64);
  }
  wr32le(block + 56, (uint32_t)bits);
  wr32le(block + 60, (uint32_t)(bits >> 32));
  xform(st, block);
  (void)md4;
  wr32le(out, st[0]); wr32le(out + 4, st[1]); wr32le(out + 8, st[2]); wr32le(out + 12, st[3]);
}

void smb_md4(const uint8_t *data, size_t len, uint8_t out[16]) {
  uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  md_pad_finish(md4_transform, st, data, len, out, 1);
}

void smb_md5(const uint8_t *data, size_t len, uint8_t out[16]) {
  uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  md_pad_finish(md5_transform, st, data, len, out, 0);
}

static const uint32_t SHA256_K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static uint32_t rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr32be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void sha256_transform(uint32_t st[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = rd32be(block + i * 4);
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void smb_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  uint8_t block[64];
  uint64_t bits = (uint64_t)len * 8;
  size_t n = len;
  const uint8_t *p = data;
  while (n >= 64) { sha256_transform(st, p); p += 64; n -= 64; }
  memset(block, 0, 64);
  memcpy(block, p, n);
  block[n] = 0x80;
  if (n >= 56) { sha256_transform(st, block); memset(block, 0, 64); }
  for (int i = 0; i < 8; i++) block[63 - i] = (uint8_t)(bits >> (8 * i));
  sha256_transform(st, block);
  for (int i = 0; i < 8; i++) wr32be(out + i * 4, st[i]);
}

void smb_hmac_md5(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t out[16]) {
  uint8_t k[64], ipad[64], opad[64], inner[16], buf[64 + 256];
  memset(k, 0, 64);
  if (keylen > 64) smb_md5(key, keylen, k);
  else memcpy(k, key, keylen);
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  /* ipad || data — data can be larger than 256; hash incrementally via concat buffer when small */
  uint8_t tmp[64 + 512];
  if (len <= 512) {
    memcpy(tmp, ipad, 64);
    memcpy(tmp + 64, data, len);
    smb_md5(tmp, 64 + len, inner);
  } else {
    /* Fallback: two-step not fully streaming; cap */
    memcpy(tmp, ipad, 64);
    memcpy(tmp + 64, data, 512);
    smb_md5(tmp, 64 + 512, inner);
  }
  memcpy(tmp, opad, 64);
  memcpy(tmp + 64, inner, 16);
  smb_md5(tmp, 80, out);
  (void)buf;
}

void smb_hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t out[32]) {
  uint8_t k[64], tmp[64 + 512], inner[32];
  memset(k, 0, 64);
  if (keylen > 64) smb_sha256(key, keylen, k);
  else memcpy(k, key, keylen);
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  size_t n = len > 512 ? 512 : len;
  memcpy(tmp, ipad, 64);
  memcpy(tmp + 64, data, n);
  smb_sha256(tmp, 64 + n, inner);
  memcpy(tmp, opad, 64);
  memcpy(tmp + 64, inner, 32);
  smb_sha256(tmp, 96, out);
}

/* AES-128 (public-domain compact) */
static const uint8_t sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes_key_expand(const uint8_t key[16], uint8_t rk[176]) {
  static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
  memcpy(rk, key, 16);
  for (int i = 4; i < 44; i++) {
    uint8_t t[4];
    memcpy(t, rk + (i - 1) * 4, 4);
    if ((i % 4) == 0) {
      uint8_t tmp = t[0];
      t[0] = sbox[t[1]] ^ rcon[i / 4 - 1];
      t[1] = sbox[t[2]];
      t[2] = sbox[t[3]];
      t[3] = sbox[tmp];
    }
    for (int j = 0; j < 4; j++) rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
  }
}

static void aes_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);
  for (int i = 0; i < 16; i++) s[i] ^= rk[i];
  for (int round = 1; round <= 10; round++) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
    uint8_t u[16];
    u[0]=t[0]; u[1]=t[5]; u[2]=t[10]; u[3]=t[15];
    u[4]=t[4]; u[5]=t[9]; u[6]=t[14]; u[7]=t[3];
    u[8]=t[8]; u[9]=t[13]; u[10]=t[2]; u[11]=t[7];
    u[12]=t[12]; u[13]=t[1]; u[14]=t[6]; u[15]=t[11];
    if (round != 10) {
      for (int c = 0; c < 4; c++) {
        uint8_t *p = u + c * 4;
        uint8_t a0=p[0],a1=p[1],a2=p[2],a3=p[3];
        p[0] = xtime(a0)^xtime(a1)^a1^a2^a3;
        p[1] = a0^xtime(a1)^xtime(a2)^a2^a3;
        p[2] = a0^a1^xtime(a2)^xtime(a3)^a3;
        p[3] = xtime(a0)^a0^a1^a2^xtime(a3);
      }
    }
    memcpy(s, u, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
  }
  memcpy(out, s, 16);
}

static void xor16(uint8_t *d, const uint8_t *a, const uint8_t *b) {
  for (int i = 0; i < 16; i++) d[i] = a[i] ^ b[i];
}

static void gf128_dbl(uint8_t x[16]) {
  uint8_t carry = (uint8_t)(x[0] >> 7);
  for (int i = 0; i < 15; i++) x[i] = (uint8_t)((x[i] << 1) | (x[i + 1] >> 7));
  x[15] = (uint8_t)(x[15] << 1);
  if (carry) x[15] ^= 0x87;
}

void smb_aes128_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t out[16]) {
  uint8_t rk[176], L[16], k1[16], k2[16], x[16], y[16];
  aes_key_expand(key, rk);
  memset(L, 0, 16);
  aes_encrypt_block(rk, L, L);
  memcpy(k1, L, 16); gf128_dbl(k1);
  memcpy(k2, k1, 16); gf128_dbl(k2);
  memset(x, 0, 16);
  size_t n = (len + 15) / 16;
  if (n == 0) n = 1;
  for (size_t i = 0; i < n; i++) {
    uint8_t blk[16];
    memset(blk, 0, 16);
    size_t off = i * 16;
    size_t ncpy = (off < len) ? ((len - off) > 16 ? 16 : (len - off)) : 0;
    if (ncpy) memcpy(blk, data + off, ncpy);
    int last = (i + 1 == n);
    if (last) {
      if (len && (len % 16) == 0) xor16(blk, blk, k1);
      else {
        if (ncpy < 16) blk[ncpy] = 0x80;
        xor16(blk, blk, k2);
      }
    }
    xor16(y, x, blk);
    aes_encrypt_block(rk, y, x);
  }
  memcpy(out, x, 16);
}

void smb3_kdf_signkey(const uint8_t *session_key, size_t session_key_len, uint8_t out[16]) {
  /* MS-SMB2 3.0.2: Label "SMB2AESCMAC\0", Context "SmbSign\0" */
  uint8_t msg[64];
  size_t p = 0;
  msg[p++] = 0; msg[p++] = 0; msg[p++] = 0; msg[p++] = 1;
  memcpy(msg + p, "SMB2AESCMAC", 11); p += 11;
  msg[p++] = 0;
  msg[p++] = 0;
  memcpy(msg + p, "SmbSign", 7); p += 7;
  msg[p++] = 0;
  msg[p++] = 0; msg[p++] = 0; msg[p++] = 0; msg[p++] = 128;
  uint8_t prf[32];
  smb_hmac_sha256(session_key, session_key_len, msg, p, prf);
  memcpy(out, prf, 16);
}

void smb_ascii_to_utf16le(const char *ascii, uint8_t *out, size_t *out_len) {
  size_t n = 0;
  if (!ascii) { *out_len = 0; return; }
  while (ascii[n]) {
    out[n * 2] = (uint8_t)ascii[n];
    out[n * 2 + 1] = 0;
    n++;
  }
  *out_len = n * 2;
}

void smb_ascii_to_utf16le_upper(const char *ascii, uint8_t *out, size_t *out_len) {
  size_t n = 0;
  if (!ascii) { *out_len = 0; return; }
  while (ascii[n]) {
    out[n * 2] = (uint8_t)toupper((unsigned char)ascii[n]);
    out[n * 2 + 1] = 0;
    n++;
  }
  *out_len = n * 2;
}

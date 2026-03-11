## MegaFlash C Library for A2osX

This document adapts the cc65 MegaFlash library to the **C environment used by A2osX**. It describes:

- The C API in `a2osx/megaflash_a2osx.h` / `.c`.
- How it maps to the MegaFlash `$C0C0` command protocol.
- How to integrate it into an A2osX build (conceptually) and call it from A2osX C.

The code is written in **portable, K&R‑style C** (no `stdint.h`, no compiler extensions) so that it can be compiled by the A2osX C compiler.

---

## 1. Files

In the MegaFlash repository:

- `a2osx/megaflash_a2osx.h` – public header for A2osX C programs.
- `a2osx/megaflash_a2osx.c` – implementation.

To use it in the A2osX project, you would typically:

1. Copy these files into an appropriate source tree under A2osX (e.g. a new `/usr/lib/libmegaflash` module).
2. Compile `megaflash_a2osx.c` with the A2osX C compiler.
3. Link the resulting object file into your C program like any other user‑level library.

Exactly how to wire it into A2osX’s `CC.S` toolchain depends on your existing build setup; the code itself is pure C and does not depend on cc65.

---

## 2. Hardware interface (background)

The library talks directly to the MegaFlash hardware registers in slot 4:

```c
#define MF_CMDREG   (*(mf_u8*)0xC0C0u)  /* command (write) / status (read) */
#define MF_STATUS   (*(mf_u8*)0xC0C0u)
#define MF_PARAM    (*(mf_u8*)0xC0C1u)  /* parameter stream */
#define MF_DATA     (*(mf_u8*)0xC0C2u)  /* data stream */
#define MF_ID       (*(mf_u8*)0xC0C3u)  /* ID / reserved */
```

Status bits:

- `MF_BUSYFLAG` (`0x80`): command in progress.
- `MF_ERRORFLAG` (`0x40`): command failed.
- `MF_ERRORCODE_MASK` (`0x1F`): Pico error code.

The helper `mf_issue_cmd(cmd)` does:

1. `MF_CMDREG = cmd;`
2. Waits while `MF_STATUS & MF_BUSYFLAG` is non‑zero.
3. Sets `mf_last_error` to the Pico error code (0 on success).

From A2osX C, you use higher‑level wrappers and do not need to manipulate these registers directly.

---

## 3. Types and error handling

### 3.1 Integer types

To avoid `stdint.h` and keep things simple, the header defines:

```c
typedef unsigned char  mf_u8;
typedef unsigned short mf_u16;
typedef unsigned long  mf_u32;
```

These are used for all public APIs instead of `uint8_t`/`uint16_t`/`uint32_t`.

### 3.2 Error state

```c
extern mf_u8 mf_last_error;
mf_u8 mf_failed(void);
```

- After any library call, `mf_last_error` holds the Pico’s error code (0 = success).
- `mf_failed()` returns non‑zero if `mf_last_error != 0`.

Many functions also return an error code directly; in those cases the return value and `mf_last_error` match.

---

## 4. Volume information

### 4.1 Volume info struct

```c
typedef struct mf_volinfo {
    mf_u8  type;          /* 0 = ProDOS, etc. */
    mf_u16 block_count;   /* 16-bit view of block count (LSB first) */
    mf_u8  medium;        /* flash / RAM / ROM code */
    mf_u8  name_len;      /* number of chars in name */
    char   name[16];      /* null-terminated name (max 15 chars) */
} mf_volinfo_t;
```

This mirrors the 21‑byte `VolInfo_t` used on the Pico, but collapses the block count to 16 bits and copies up to 15 chars of the volume name, null‑terminated in `name[15]`.

### 4.2 `mf_u8 mf_get_unit_count(void);`

**Description**

Return the number of SmartPort units currently exposed by MegaFlash.

**Signature**

```c
mf_u8 mf_get_unit_count(void);
```

**Returns**

- `0..15` – number of units, on success.
- `0xFF` – on error; check `mf_last_error`.

**Usage**

```c
#include "megaflash_a2osx.h"

void show_units(void)
{
    mf_u8 n = mf_get_unit_count();
    if (mf_failed()) {
        /* print or log mf_last_error using A2osX libc */
        return;
    }
    /* loop over units as needed */
}
```

### 4.3 `mf_u8 mf_get_volinfo(mf_u8 unitNum, mf_volinfo_t* info);`

**Description**

Fetch volume information for the given unit into `*info`.

**Signature**

```c
mf_u8 mf_get_volinfo(mf_u8 unitNum, mf_volinfo_t* info);
```

**Parameters**

- `unitNum`: SmartPort unit (typically 1‑based).
- `info`: pointer to an `mf_volinfo_t` filled on success.

**Returns**

- `1` – success.
- `0` – failure; `mf_last_error` is non‑zero.

**Usage**

```c
mf_volinfo_t vi;
if (mf_get_volinfo(4, &vi)) {
    /* e.g. use LIBC PrintF to show it */
    /* PrintF("U4: %s (%u blocks)\n", vi.name, vi.block_count); */
} else {
    /* PrintF("GetVolInfo failed, err=%u\n", mf_last_error); */
}
```

---

## 5. Block I/O

These functions move exactly one 512‑byte block per call.

### 5.1 `mf_u8 mf_read_block(mf_u8 unitNum, mf_u32 block, void* buffer);`

**Description**

Read one 512‑byte ProDOS block into `buffer`.

**Signature**

```c
mf_u8 mf_read_block(mf_u8 unitNum, mf_u32 block, void* buffer);
```

**Parameters**

- `unitNum`: SmartPort unit number.
- `block`: 24‑bit block index (0‑based). Only the low 24 bits of `mf_u32` are used.
- `buffer`: pointer to a 512‑byte buffer.

**Returns**

- `0` – success.
- Non‑zero – Pico error code; also stored in `mf_last_error`.

**Usage**

```c
static mf_u8 blockbuf[512];

mf_u8 err = mf_read_block(4, 0, blockbuf);  /* unit 4, block 0 */
if (err) {
    /* PrintF("Read error %u\n", err); */
} else {
    /* blockbuf now holds block data */
}
```

### 5.2 `mf_u8 mf_write_block(mf_u8 unitNum, mf_u32 block, const void* buffer);`

**Description**

Write one 512‑byte block from `buffer` to the given unit/block.

**Signature**

```c
mf_u8 mf_write_block(mf_u8 unitNum, mf_u32 block, const void* buffer);
```

**Parameters**

- `unitNum`: SmartPort unit number.
- `block`: 24‑bit block index (0‑based).
- `buffer`: pointer to 512 bytes of data to write.

**Returns**

- `0` – success.
- Non‑zero – Pico error code (`mf_last_error`).

**Notes**

- The library automatically writes the write‑enable key (`MF_WE_KEY`) into the parameter stream before issuing `CMD_WRITEBLOCK`.

**Usage**

```c
mf_u8 i;
static mf_u8 blockbuf[512];

for (i = 0; i < 512; ++i) {
    blockbuf[i] = 0;
}
/* e.g. copy a string into the start of the block */
/* strcpy((char*)blockbuf, "HELLO FROM A2osX + MegaFlash"); */

if (mf_write_block(4, 0, blockbuf) != 0) {
    /* PrintF("Write error %u\n", mf_last_error); */
}
```

---

## 6. Firmware and time strings

### 6.1 `void mf_get_firmware_version(char* out12);`

**Description**

Fetch a 12‑byte firmware version string from MegaFlash into `out12`.

**Signature**

```c
void mf_get_firmware_version(char* out12);
```

**Parameters**

- `out12`: pointer to at least 12 bytes of writable memory.

**Notes**

- On error, `out12[0]` is set to `'\0'` and `mf_last_error` is non‑zero.
- The Pico does not terminate the string; if you want a C string, add `out12[12] = '\0';` yourself.

**Usage**

```c
char ver[13];

mf_get_firmware_version(ver);
if (mf_failed()) {
    /* PrintF("GETFIRMWAREVER failed %u\n", mf_last_error); */
} else {
    ver[12] = '\0';
    /* PrintF("MegaFlash firmware: %s\n", ver); */
}
```

### 6.2 `void mf_get_time_string(char* out8);`

**Description**

Fetch an 8‑byte ASCII time string (e.g. `"11:50 AM"`) into `out8`.

**Signature**

```c
void mf_get_time_string(char* out8);
```

**Parameters**

- `out8`: pointer to at least 8 bytes of writable memory.

**Notes**

- On error, `out8[0]` is set to `'\0'` and `mf_last_error` is non‑zero.
- As with the version string, you may want to append a terminator at `out8[8]`.

**Usage**

```c
char t[9];

mf_get_time_string(t);
if (mf_failed()) {
    /* PrintF("GETTIMESTR failed %u\n", mf_last_error); */
} else {
    t[8] = '\0';
    /* PrintF("Time: %s\n", t); */
}
```

---

## 7. ROM disk control

These wrappers toggle MegaFlash’s ROM disk visibility and position.

### 7.1 `mf_u8 mf_enable_romdisk_last(void);`

**Description**

Enable the ROM disk and position it as the **last SmartPort unit**.

```c
mf_u8 mf_enable_romdisk_last(void);
```

**Returns**

- `0` – success.
- Non‑zero – Pico error code (`mf_last_error`).

### 7.2 `mf_u8 mf_enable_romdisk_first(void);`

**Description**

Enable the ROM disk and position it as the **first SmartPort unit**, suitable for “boot from ROM disk” use.

```c
mf_u8 mf_enable_romdisk_first(void);
```

**Returns**

- `0` – success.
- Non‑zero – Pico error code.

### 7.3 `mf_u8 mf_disable_romdisk(void);`

**Description**

Hide the ROM disk so it does not appear as a SmartPort unit.

```c
mf_u8 mf_disable_romdisk(void);
```

**Returns**

- `0` – success.
- Non‑zero – Pico error code.

**Usage**

```c
if (mf_enable_romdisk_last() != 0) {
    /* PrintF("Enable ROM disk failed: %u\n", mf_last_error); */
}
```

---

## 8. WiFi self-test

### 8.1 `mf_u8 mf_test_wifi(void);`

**Description**

Run MegaFlash’s WiFi self‑test and return its result code.

```c
mf_u8 mf_test_wifi(void);
```

**Returns**

- `0` – test passed.
- Non‑zero – test failed or not implemented (device‑specific codes).
- On protocol error, `mf_last_error` is also non‑zero.

**Usage**

```c
mf_u8 r = mf_test_wifi();

if (mf_failed()) {
    /* PrintF("TESTWIFI protocol failure %u\n", mf_last_error); */
} else if (r != 0) {
    /* PrintF("TESTWIFI reported error code %u\n", r); */
} else {
    /* PrintF("WiFi OK\n"); */
}
```

---

## 9. Relationship to A2osX C and libc

The A2osX kernel already integrates MegaFlash indirectly via:

- **ProDOS / SmartPort** drivers for storage (block devices).
- **Clock driver** for time/date (ProDOS `GET_TIME` / `SET_TIME`).
- **Applesoft FPU hooks** when running under standard ROM.

The library here is intended for **user‑level C programs** that want:

- Low‑level control over MegaFlash block devices (outside the filesystem).
- Direct access to firmware metadata and timers.
- Programmatic control over ROM disk visibility and WiFi tests.

It does **not** depend on A2osX’s kernel API (`>LIBC`, `>KAPI`, etc.); it only assumes:

- The process is running on an Apple IIc/IIc+ with MegaFlash in **slot 4**.
- It can read/write the `$C0C0`–`$C0C3` soft switches directly.

For integration with the rest of A2osX (e.g. using `LIBC PrintF`), treat this as a normal user library:

1. Compile `megaflash_a2osx.c` with A2osX’s C compiler.
2. Link the resulting object with your program.
3. Use A2osX libc functions (e.g. `PrintF`, `FOpen`, etc.) for I/O and debug output around the MegaFlash calls.


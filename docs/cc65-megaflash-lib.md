## MegaFlash cc65 Library

This library gives cc65 programs (Apple IIc / IIc+) a small, C‑friendly wrapper around the **MegaFlash** `$C0C0` command interface:

- Simple functions for **unit enumeration**, **volume info**, and **block I/O**.
- Helpers to read the **firmware version** and **time string**.
- Convenience wrappers to **enable/disable the ROM disk** and **test WiFi**.

The implementation lives in:

- `cc65/megaflash.h`
- `cc65/megaflash.c`

You can either:

- Compile them directly into your project, or
- Build a static library (e.g. `megaflash.lib`) and link against it.

---

## 1. Building and linking

### 1.1 Quick example: single C source

```bash
cl65 -t apple2 -O -o demo demo.c cc65/megaflash.c
```

This:

- Targets **apple2** (IIc / IIc+ are compatible),
- Optimizes (`-O`),
- Links `demo.c` and `megaflash.c` into the binary `demo`.

### 1.2 Separating compilation and link

```bash
cc65 -t apple2 -O -c cc65/megaflash.c
cc65 -t apple2 -O -c demo.c
ld65 -C apple2.cfg -o demo demo.o megaflash.o apple2.lib
```

Or create `megaflash.lib` with `ar65` and link it like any other library.

---

## 2. Low-level interface (background)

The library talks directly to MegaFlash’s four soft‑switches in the slot‑4 C0 area:

```c
#define MF_CMDREG   (*(volatile uint8_t*)0xC0C0u)  /* command (write) / status (read) */
#define MF_STATUS   (*(volatile uint8_t*)0xC0C0u)
#define MF_PARAM    (*(volatile uint8_t*)0xC0C1u)  /* parameter stream */
#define MF_DATA     (*(volatile uint8_t*)0xC0C2u)  /* data stream */
#define MF_ID       (*(volatile uint8_t*)0xC0C3u)  /* ID / reserved */
```

The Pico sets these bits in the status byte:

- `MF_BUSYFLAG` (`0x80`): command in progress.
- `MF_ERRORFLAG` (`0x40`): command failed.
- `MF_ERRORCODE_MASK` (`0x1F`): Pico error code.

The helper `mf_issue_cmd(cmd)`:

1. Writes `cmd` to `MF_CMDREG`.
2. Busy‑waits while `MF_STATUS & MF_BUSYFLAG` is non‑zero.
3. Sets the global `mf_last_error` to the Pico error code (0 on success).

You normally don’t call `mf_issue_cmd` yourself; use the higher‑level functions below.

---

## 3. Error handling

```c
extern uint8_t mf_last_error;
uint8_t __fastcall__ mf_failed(void);
```

- After any library call, `mf_last_error` holds the Pico’s error code (0 on success).
- `mf_failed()` is a convenience wrapper returning non‑zero if `mf_last_error != 0`.

For functions that already return an error code (e.g. `mf_read_block`), that return value and `mf_last_error` are the same.

---

## 4. Volume information

### 4.1 `mf_volinfo_t`

```c
typedef struct mf_volinfo {
    uint8_t  type;          /* 0 = ProDOS, etc. */
    uint16_t block_count;   /* 16-bit view of block count (LSB first) */
    uint8_t  medium;        /* flash / RAM / ROM code */
    uint8_t  name_len;      /* number of chars in name */
    char     name[16];      /* null-terminated name (max 15 chars) */
} mf_volinfo_t;
```

This is a compact view of the 21‑byte `VolInfo_t` struct used inside the firmware. The library:

- Copies the 16‑bit block count, medium, and name length as reported by the Pico.
- Copies up to 15 bytes of the volume name and null‑terminates it in `name[15]`.

### 4.2 `uint8_t mf_get_unit_count(void);`

**Description**

Return the number of SmartPort units currently exposed by MegaFlash.

**Signature**

```c
uint8_t __fastcall__ mf_get_unit_count(void);
```

**Returns**

- `0..15` – number of units, on success.
- `0xFF` – on error; inspect `mf_last_error` for the Pico error code.

**Usage**

```c
#include "cc65/megaflash.h"

void main(void)
{
    uint8_t n = mf_get_unit_count();
    if (mf_failed()) {
        /* handle error, e.g. print mf_last_error */
    } else {
        /* iterate units 1..n (or 0..n-1 depending on your convention) */
    }
}
```

### 4.3 `uint8_t mf_get_volinfo(uint8_t unitNum, mf_volinfo_t* info);`

**Description**

Fetch the volume information for a given SmartPort unit and store it in an `mf_volinfo_t`.

**Signature**

```c
uint8_t __fastcall__ mf_get_volinfo(uint8_t unitNum, mf_volinfo_t* info);
```

**Parameters**

- `unitNum`: SmartPort unit number (usually 1‑based).
- `info`: pointer to a caller‑allocated `mf_volinfo_t`.

**Returns**

- `1` – success (info filled in).
- `0` – failure; `mf_last_error` holds the Pico error code.

**Usage**

```c
mf_volinfo_t vi;
if (mf_get_volinfo(4, &vi)) {
    cprintf("Unit 4: %s (%u blocks)\r\n", vi.name, vi.block_count);
} else {
    cprintf("GetVolInfo failed, err=%u\r\n", mf_last_error);
}
```

---

## 5. Block I/O

These wrap the raw `CMD_READBLOCK` / `CMD_WRITEBLOCK` commands and move exactly 512 bytes per call.

### 5.1 `uint8_t mf_read_block(uint8_t unitNum, uint32_t block, void* buffer);`

**Description**

Read one 512‑byte block from a unit into `buffer`.

**Signature**

```c
uint8_t __fastcall__ mf_read_block(uint8_t unitNum,
                                   uint32_t block,
                                   void*    buffer);
```

**Parameters**

- `unitNum`: SmartPort unit number.
- `block`: 24‑bit ProDOS block number (0‑based). Only the low 24 bits of the `uint32_t` are used.
- `buffer`: pointer to a 512‑byte buffer.

**Returns**

- `0` – success.
- Non‑zero – Pico error code; also stored in `mf_last_error`.

**Notes**

- The library resets the MegaFlash parameter/data pointers before issuing the command.
- It copies 512 bytes from `MF_DATA` into `buffer`.

**Usage**

```c
static unsigned char blockbuf[512];

uint8_t err = mf_read_block(4, 0, blockbuf);  /* unit 4, block 0 */
if (err) {
    cprintf("Read failed: err=%u\r\n", err);
} else {
    /* blockbuf now contains the block */
}
```

### 5.2 `uint8_t mf_write_block(uint8_t unitNum, uint32_t block, const void* buffer);`

**Description**

Write one 512‑byte block from `buffer` to the given unit/block.

**Signature**

```c
uint8_t __fastcall__ mf_write_block(uint8_t unitNum,
                                    uint32_t block,
                                    const void* buffer);
```

**Parameters**

- `unitNum`: SmartPort unit number.
- `block`: 24‑bit block number (0‑based).
- `buffer`: pointer to 512 bytes of data to write.

**Returns**

- `0` – success.
- Non‑zero – Pico error code; also stored in `mf_last_error`.

**Implementation details**

- Resets pointer state.
- Writes `unitNum` and the 3‑byte block number into the parameter stream.
- Streams 512 bytes from `buffer` into `MF_DATA`.
- Writes the write‑enable key (`MF_WE_KEY`) into `MF_PARAM`.
- Issues `MF_CMD_WRITEBLOCK`.

**Usage**

```c
memset(blockbuf, 0, sizeof(blockbuf));
strcpy((char*)blockbuf, "HELLO FROM MEGAFLASH");
if (mf_write_block(4, 0, blockbuf) != 0) {
    cprintf("Write error %u\r\n", mf_last_error);
}
```

---

## 6. Firmware and time strings

### 6.1 `void mf_get_firmware_version(char* out12);`

**Description**

Fetch a 12‑byte firmware version string (`"V1.1.19-eo "` style) into `out12`.

**Signature**

```c
void __fastcall__ mf_get_firmware_version(char* out12);
```

**Parameters**

- `out12`: pointer to at least 12 bytes of writable memory.

**Notes**

- On error, `out12[0]` is set to `'\0'` and `mf_last_error` is non‑zero.
- The string is **not null‑terminated** by the Pico; the wrapper copies exactly 12 bytes and leaves termination to the caller if desired.

**Usage**

```c
char ver[13];

mf_get_firmware_version(ver);
if (mf_failed()) {
    cprintf("GETFIRMWAREVER failed, err=%u\r\n", mf_last_error);
} else {
    ver[12] = '\0';
    cprintf("MegaFlash firmware: %s\r\n", ver);
}
```

### 6.2 `void mf_get_time_string(char* out8);`

**Description**

Fetch an 8‑byte ASCII time string (e.g. `"11:50 AM"`) into `out8`.

**Signature**

```c
void __fastcall__ mf_get_time_string(char* out8);
```

**Parameters**

- `out8`: pointer to at least 8 bytes of writable memory.

**Notes**

- On error, `out8[0]` is set to `'\0'` and `mf_last_error` is non‑zero.
- The string is exactly 8 bytes; you may want to add a terminator at `out8[8]`.

**Usage**

```c
char t[9];

mf_get_time_string(t);
if (mf_failed()) {
    cprintf("GETTIMESTR failed, err=%u\r\n", mf_last_error);
} else {
    t[8] = '\0';
    cprintf("Time: %s\r\n", t);
}
```

---

## 7. ROM disk control

These wrap the `CMD_ENABLEROMDISK` and `CMD_DISABLEROMDISK` commands.

### 7.1 `uint8_t mf_enable_romdisk_last(void);`

**Description**

Enable the ROM disk and position it as the **last SmartPort unit**.

**Signature**

```c
uint8_t __fastcall__ mf_enable_romdisk_last(void);
```

**Returns**

- `0` – success.
- Non‑zero – Pico error code (`mf_last_error`).

### 7.2 `uint8_t mf_enable_romdisk_first(void);`

**Description**

Enable the ROM disk and position it as the **first SmartPort unit** (suitable for “boot from ROM disk” scenarios).

**Signature**

```c
uint8_t __fastcall__ mf_enable_romdisk_first(void);
```

**Returns**

- `0` – success.
- Non‑zero – Pico error code.

### 7.3 `uint8_t mf_disable_romdisk(void);`

**Description**

Hide the ROM disk from SmartPort so it does not appear as a unit.

**Signature**

```c
uint8_t __fastcall__ mf_disable_romdisk(void);
```

**Returns**

- `0` – success.
- Non‑zero – Pico error code.

**Usage**

```c
if (mf_enable_romdisk_last() != 0) {
    cprintf("Enable ROM disk failed, err=%u\r\n", mf_last_error);
}
```

---

## 8. WiFi self-test

### 8.1 `uint8_t mf_test_wifi(void);`

**Description**

Run the MegaFlash WiFi self‑test and return its result code.

**Signature**

```c
uint8_t __fastcall__ mf_test_wifi(void);
```

**Returns**

- `0` – test passed.
- Non‑zero – test failed or not implemented (device‑specific codes).
- On protocol‑level error, `mf_last_error` is also non‑zero.

**Usage**

```c
uint8_t r = mf_test_wifi();
if (mf_failed()) {
    cprintf("TESTWIFI failed at protocol level, err=%u\r\n", mf_last_error);
} else if (r != 0) {
    cprintf("TESTWIFI reported failure code %u\r\n", r);
} else {
    cprintf("WiFi test OK\r\n");
}
```

---

## 9. FPU API (MBF-level)

MegaFlash’s FPU hardware is wired to emulate **Applesoft BASIC**’s floating point format (MBF). The Pico expects operands in a 13‑byte buffer representing Applesoft’s FAC and ARG, and returns a 1‑byte error code followed by a 7‑byte MBF result.

The cc65 `megaflash` library exposes this protocol at the **MBF byte level** via two small types and a set of functions. It does **not** attempt to convert between C `float`/`double` and MBF; that conversion can be done in user code if needed.

### 9.1 Data structures

```c
typedef struct mf_fpu_args {
    uint8_t bytes[13];
} mf_fpu_args_t;

typedef struct mf_fpu_result {
    uint8_t bytes[8];      /* [0] = error, [1..7] = MBF value */
} mf_fpu_result_t;
```

**Input layout (`mf_fpu_args_t.bytes`):**

The 13 bytes encode Applesoft FAC and ARG in this order (see comments in `pico/fpu.c`):

```text
Index  Field
-----  -----------------------------------------
  0    FACSIGN      (FAC sign byte, $A2)
  1    ARGSIGN      (ARG sign byte, $AA)
  2    FACMANT4     (FAC mantissa byte 4, $A1)
  3    ARGMANT4     (ARG mantissa byte 4, $A9)
  4    FACMANT3     (FAC mantissa byte 3, $A0)
  5    ARGMANT3     (ARG mantissa byte 3, $A8)
  6    FACMANT2     (FAC mantissa byte 2, $9F)
  7    ARGMANT2     (ARG mantissa byte 2, $A7)
  8    FACMANT1     (FAC mantissa byte 1, $9E)
  9    ARGMANT1     (ARG mantissa byte 1, $A6)
 10    FACEXP       (FAC exponent, $9D)
 11    ARGEXP       (ARG exponent, $A5)
 12    FACEXT       (FAC extension, $AC)
```

These fields are exactly what the firmware’s `fpu_exec` routine sends when it intercepts Applesoft FPU calls.

**Output layout (`mf_fpu_result_t.bytes`):**

```text
Index  Field
-----  ---------------------------------------------
  0    error flags:
         bit 7 = OVERFLOWERROR
         bit 6 = DIV0ERROR
         bit 5 = IQERROR
  1    sign           (MSB set if negative)
  2    mantissa 4
  3    mantissa 3
  4    mantissa 2
  5    mantissa 1 (MSB always set)
  6    exponent
  7    extension
```

### 9.2 Core FPU call

```c
void mf_fpu_op(uint8_t cmd,
               const mf_fpu_args_t* args,
               mf_fpu_result_t*     res);
```

**Description**

Send a 13‑byte operand buffer (`args`) to MegaFlash, execute the FPU command (`cmd`), and read back an 8‑byte result (`res`).

**Parameters**

- `cmd`: one of the FPU command codes:
  - `MF_CMD_FADD`  (`0x30`)
  - `MF_CMD_FMUL`  (`0x31`)
  - `MF_CMD_FDIV`  (`0x32`)
  - `MF_CMD_FSIN`  (`0x33`)
  - `MF_CMD_FCOS`  (`0x34`)
  - `MF_CMD_FTAN`  (`0x35`)
  - `MF_CMD_FATN`  (`0x36`)
  - `MF_CMD_FLOG`  (`0x37`)
  - `MF_CMD_FEXP`  (`0x38`)
  - `MF_CMD_FSQR`  (`0x39`)
  - `MF_CMD_FOUT`  (`0x3A`)
- `args`: pointer to an `mf_fpu_args_t` filled with FAC/ARG bytes.
- `res`: pointer to an `mf_fpu_result_t` which receives the result.

**Error handling**

- On protocol failure, `mf_last_error` is non‑zero and `res->bytes` contents are undefined.
- On success (`mf_last_error == 0`), `res->bytes[0]` is the arithmetic error flags; 0 means “no FPU error”.

### 9.3 Convenience wrappers

For each FPU operation there is a convenience wrapper:

```c
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
```

Each simply calls `mf_fpu_op` with the appropriate `MF_CMD_F*` code.

### 9.4 Using FPU from cc65

Because cc65’s `float`/`double` format is **not** Applesoft MBF, the library does **not** attempt to convert C floats to MBF. Instead:

- You treat `mf_fpu_args_t` and `mf_fpu_result_t` as **opaque MBF containers**.
- You can:
  - Build MBF operands in assembly and pass them into C as `mf_fpu_args_t`.
  - Or write your own conversion between cc65’s floats and MBF if you need a pure‑C interface.

Example skeleton (MBF construction omitted):

```c
mf_fpu_args_t   a;
mf_fpu_result_t r;

/* TODO: fill a.bytes[0..12] with MBF representation of FAC/ARG */

mf_fadd(&a, &r);
if (mf_last_error != 0) {
    /* protocol failure */
} else if (r.bytes[0] != 0) {
    /* FPU arithmetic error: OVERFLOW/DIV0/IQERROR bits in r.bytes[0] */
} else {
    /* r.bytes[1..7] now hold MBF result */
}
```

If you later add helper routines (in C or assembly) to convert between cc65 floats and MBF, you can layer them on top of this MBF‑level API without changing the underlying library.

---

## 10. Relationship to ROM-patched features

The library talks directly to the C0C0 protocol. Many features are also exposed indirectly via ROM patches:

- **Block devices**: ProDOS’s MLI `READ_BLOCK` / `WRITE_BLOCK` and SmartPort `SP_READBLOCK` / `SP_WRITEBLOCK` are already wired to MegaFlash; use them for normal filesystem I/O.
- **Clock**: The ProDOS clock driver is patched to use `CMD_GETPRODOSTIME` / `CMD_SETRTC_PRODOS`. High‑level code should call `GET_TIME` / `SET_TIME` instead of C0C0 unless you need low‑level control.
- **FPU**: Applesoft math (`SIN`, `COS`, `TAN`, `ATN`, `LOG`, `EXP`, `SQR`, arithmetic) is automatically accelerated when FPU support is enabled; the library deliberately does not duplicate the FPU interface, since the ROM hooks are the preferred integration point.

Use this library when you:

- Are writing cc65 C programs that need **direct control** over MegaFlash features (e.g. your own formatter, diagnostics, or block tools), or
- Are writing a new language/runtime on top of cc65 and want a small, clean C API instead of hand‑coding `$C0C0` sequences.


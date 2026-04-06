# Flash validation (Apple II / Applesoft)

`FLASHVAL.BAS` exercises the **same MegaFlash register path** the Control Panel uses (`$C0C0`–`$C0C3`): command, parameter stream, and data buffer. It is intended to:

1. **Characterize** a known-good board + firmware + flash (and optionally a specific ProDOS volume layout).
2. **Save a baseline** text file of checksums and small metadata.
3. **Compare** a later run (new flash chip, new firmware, new build) against that baseline.

## Requirements

- **Applesoft** (IIe, IIc, IIgs in IIe mode, etc.).
- MegaFlash in a standard slot; **default slot 4** → base address **`$C0C0`**.
- **ProDOS BASIC** recommended for **baseline** and **compare** modes (sequential text file I/O). Mode **1** (screen only) works without ProDOS.

## Register layout (same as `common/defines.inc`)

| Offset from slot base | Address (slot 4) | Role |
|----------------------|------------------|------|
| +0 | `$C0C0` | Write = command; read = status (bit 7 = busy, bit 6 = error, bits 0–4 = MegaFlash error code) |
| +1 | `$C0C1` | Parameter byte stream (auto-advancing pointer) |
| +2 | `$C0C2` | Data byte stream (512-byte buffer, auto-advancing) |

Slot base in decimal: **`49280 + 16 * SLOT`** (e.g. slot 4 → **49344**).

## What the script does (non-destructive)

Default suite **does not format, erase, or write blocks**. It:

1. **`CMD_MODELINEAR` (`$03`)** — linear data buffer mode.
2. **`CMD_GETDEVINFO` (`$10`)** — checksums parameter bytes **0–11** only (excludes firmware build timestamp at **12–15**).
3. Optional line **`FWVER`** — checksum of bytes **3–4** only (firmware version word); *expect this to change when you intentionally upgrade UF2*.
4. **`CMD_GETDEVSTATUS` (`$11`)** — unit count byte.
5. **`CMD_GETUNITSTATUS` (`$12`)** — unit **1**, three block-count bytes.
6. **`CMD_GETDIB` (`$13`)** — unit **1**, **25** DIB bytes.
7. **`CMD_GETVOLINFO` (`$14`)** — unit **1**; checksum **32** parameter bytes (volume name affects the value).
8. **`CMD_READBLOCK` (`$15`)** — unit **1**, blocks **0** and **1**: status nibble, `parameterBuffer[0]` (SmartPort-style result), **16-bit checksum** of 512 data bytes.

Destructive tests (**`CMD_FORMATDISK` / `CMD_ERASEDISK` / `CMD_WRITEBLOCK`**) are **not** run here; add a separate harness if you need them, with write-enable key **`$71`** per `common/defines.h`.

## Baseline file format (`FLASHVAL1`)

Sequential text file:

1. Header: **`FLASHVAL1`**
2. Slot: decimal **1–7**
3. One line per metric: **`KEY VALUE`** (value = decimal integer; checksums are **16-bit**, 0–65535)

Keys emitted:

| Key | Meaning |
|-----|---------|
| `DEVINFO12` | Sum mod 65536 of device info bytes 0–11 |
| `FWVER` | Sum mod 65536 of bytes 3–4 (version only) |
| `DEVSTATUS` | First parameter byte after `CMD_GETDEVSTATUS` |
| `U1BC` | Sum mod 65536 of 3 unit-status bytes |
| `DIB25` | Sum mod 65536 of 25 DIB bytes |
| `VOL32` | Sum mod 65536 of 32 volume-info parameter bytes |
| `RB1_0_ST` | Status byte (`PEEK` command/status register) after `CMD_READBLOCK` |
| `RB1_0_PE` | First parameter byte after read (SmartPort-style result; 0 = success) |
| `RB1_0_DC` | Sum mod 65536 of 512 data bytes |
| `RB1_1_*` | Same for unit 1, block 1 |

**Note:** `VOL32` and block data checksums depend on **disk content** and **enabled units**. Capture baseline on a **stable** image (or document that those lines are “environment-specific”).

## Interpreting mismatches

- **`DEVINFO12` / `U1BC` / `DIB25`**: strong indicators of **flash size / geometry / ID** path (e.g. different chip or `InitFlash` behaviour).
- **`FWVER`**: changes on every firmware version bump — ignore or refresh baseline when updating UF2.
- **`RB1_0` / `RB1_1`**: if device info matches but these differ, suspect **media/image** differences, not necessarily SPI flash silicon.

## Source of truth for command values

`common/defines.h` / `common/defines.inc` (`CMD_*`), and `pico/cmdhandler.c` (`DoGetDeviceInfo`, `DoReadBlock`, …).

## Bootable disk (`FLASHVALID.dsk`)

Requires **Java** and [AppleCommander](https://applecommander.github.io/) (CLI jar). Default jar path: **`$HOME/Library/Application Support/AppleCommander/AppleCommander-ac-13.0.jar`** — override with **`AC_JAR`**.

From the repo root (or run the script from any cwd; it resolves paths relative to the script):

```bash
./tools/flash-validate/build-flashval-disk.sh
```

Writes **`tools/flash-validate/FLASHVALID.dsk`**. **`OUT=`** and **`ROMDISK=`** override output path and romdisk image.

Contents:

- **PRODOS** / **BASIC.SYSTEM** — copied raw from **`pico/romdisk.po`** (the same image embedded in firmware).
- **FLASHVAL** — tokenized Applesoft from **`FLASHVAL.DSK.BAS`** (screen-only test suite; run under BASIC.SYSTEM).
- **FLASHVAL.SRC** — text copy of **`FLASHVAL.BAS`** (full program including ProDOS baseline/compare); use for reference or manual entry; AppleCommander **`-bas`** on the full file is still unreliable.

**Note:** **`PR`** is not a safe variable name in Applesoft (it abbreviates **PRINT**). This tree uses **`PX`** and **`D1`** for the parameter and data ports at **`CR+1`** and **`CR+2`**.

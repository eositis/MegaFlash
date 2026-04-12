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

## Overnight soak tool (`FLASHSOAK`)

`FLASHSOAK.BAS` is a destructive, long-running validator intended to run overnight and emit parseable CSV rows. On screen, **line 21** shows the current phase (cycle, unit, format, file I/O, checksum block progress every 256 blocks, TFTP wait ticks every 50 polls, pass/fail), so you can follow progress without a serial console.

Per cycle, for each writable MegaFlash volume (including RAM disk when enabled), it:

- Formats the unit.
- Creates/appends/deletes files, plus a fill-to-error pass.
- Computes a whole-volume checksum (sum of all bytes from `CMD_READBLOCK`).
- Uploads image to TFTP server `192.168.0.10` as `validationX.po` (X = unit number).
- Reformats unit, downloads `validationX.po` back into place.
- Recomputes checksum and verifies it matches pre-upload value.
- Logs each step to a **ProDOS text** (SEQ) file — default **`SOAK.TXT`** — with **comma-separated** lines (`ts,cycle,unit,event,result,v1,v2`). Use a **`.TXT`** name so ProDOS/BASIC treat it as a normal ASCII text file; the content is still CSV-style for import into spreadsheets. **`ts`** is **8 characters** from ProDOS **`TIME$`** (host clock). The log file is created or opened at the **start of cycle 1** (after the **PROGRESS** banner), not during the initial prompts — so you can tell whether a hang is in **ProDOS `OPEN`** (line 21 shows **`OPENING …`**) or in the soak steps. Type **`NONE`** (uppercase or lowercase) at the log prompt to **disable the log file** entirely (destructive test still runs). **Note:** Applesoft only distinguishes the **first two characters** of variable names, so **`LGSKIP`** and **`LGINIT`** would alias each other; the program uses **`Q1`**/**`Q2`** for skip and log-init flags. **`OPEN`** / **`WRITE`** use the same string pattern as **`FLASHVAL.BAS`** (no extra space before the filename). If the **default volume** is wrong, use a full path (e.g. **`/FLASHVALID/SOAK.TXT`** or **`/RAM/SOAK.TXT`**). If **`OPEN`** appears to **hang** (status line stuck on **`OPENING …`**), the MLI is usually waiting on the wrong volume or a slow path — try **`/RAM/SOAK.TXT`**, set **`PREFIX`** to the volume you are writing to, or use **`NONE`** to run without a log. The log file is **never deleted**: each program run appends a **`RUN_START`** row first, then all events for that session. If the file does not exist yet, the first **`OPEN`** creates it and writes the header row.

If all units pass, it reformats all writable units again and starts the next cycle.

**Files:**

- `FLASHSOAK.BAS` - source for the overnight test.
- On disk image, `FLASHSOAK` (tokenized) and `FLASHSOAK.SRC` (text) are both included.

## Bootable disk (`FLASHVALID.po`)

Requires **Java** and [AppleCommander](https://applecommander.github.io/) (CLI jar). The build follows the same pattern as **`../a2speed/Makefile`** `disk` target: create a **ProDOS 140K** image with **`-pro140`**, then add files with **`-p`**, **`-bas`**, **`-ptx`**.

- Default jar: **`$HOME/Library/Application Support/AppleCommander/AppleCommander-ac.jar`** (same default name as a2speed). Override with **`AC_JAR`**.
- On Apple Silicon, the script prefers **`/opt/homebrew/opt/openjdk/bin/java`** when present (avoids Intel **`java`** “Bad CPU type” issues), same as a2speed.

From the repo root:

```bash
./tools/flash-validate/build-flashval-disk.sh
```

Or:

```bash
cd tools/flash-validate && make disk
```

Writes **`tools/flash-validate/FLASHVALID.po`** (**143360 bytes**, same **`.po`** convention as **`../a2speed`**’s **`a2speed.po`**; ProDOS raw block order). **`OUT=`** overrides the output path. **`SYS_SRC=`** overrides the source image for **PRODOS** / **BASIC.SYSTEM** (default **`cpanel/prodos19.dsk`**).

Contents:

- **PRODOS** / **BASIC.SYSTEM** — copied raw from **`cpanel/prodos19.dsk`** (valid 140K ProDOS 1.9 image shipped with the Control Panel build inputs).
- **TFTPUTIL** — tokenized Applesoft from **`TFTPUTIL.BAS`**: standalone TFTP **upload** / **download** of a disk image to/from a MegaFlash unit (same **`CMD_TFTPRUN`** / **`CMD_TFTPSTATUS`** path as the Control Panel’s TFTP feature; **Pico W** + WiFi required). **`TFTPUTIL.SRC`** is the text source.
- **TFTPUTIL.DOC** — on-disk text documentation from **`TFTPUTIL.TXT`** (startup behavior, host config, prompts, and error notes).
- **FLASHVAL** — tokenized Applesoft from **`FLASHVAL.DSK.BAS`** (screen-only test suite; run under BASIC.SYSTEM).
- **FLASHSOAK** — tokenized Applesoft overnight stress validator.
- **FLASHVAL.SRC** — text copy of **`FLASHVAL.BAS`** (full program including ProDOS baseline/compare); use for reference or manual entry; AppleCommander **`-bas`** on the full file is still unreliable.
- **FLASHSOAK.SRC** — text copy of `FLASHSOAK.BAS`.
- **WGET65V** (optional) — when **`tools/wget65-verbose/wget65v.bin`** is built and **`build-flashval-disk.sh`** can find `cl65` and **`apple2enh/util/loader.system`**, the image also includes the **`WGET65V`** binary, **`WGET65V.SYSTEM`** loader, **`WGET65V`** Applesoft launcher, **`WGET65V.SRC`**, and **`WGET65V.DOC`**. If the binary is missing, **`WGET65V.DOC`** alone is added with build instructions. See **`tools/wget65-verbose/README.md`**.

**Note:** **`PR`** is not a safe variable name in Applesoft (it abbreviates **PRINT**). This tree uses **`PX`** and **`D1`** for the parameter and data ports at **`CR+1`** and **`CR+2`**.

## WGET65V (verbose wget / ip65 fork)

**`tools/wget65-verbose/`** is a fork of upstream **`wget65`** with **screen** logging for each Uthernet II / W5100 handshake step and **register dumps** (MR, RTR XOR, RMSR, PTIMER). Build **`wget65v.bin`** with cc65 and a stock **`ip65`** checkout, then rebuild **`FLASHVALID.po`** so the disk includes the binary and launcher. Details: **`tools/wget65-verbose/README.md`**.

## TFTP utility (`TFTPUTIL`)

Run **`TFTPUTIL`** from BASIC.SYSTEM. It starts in **80-column mode**, auto-detects MegaFlash slot (prefers slot 4, otherwise scans 1-7), then prompts for **TFTP host FQDN or IP** (default loaded/saved via `TFTPUTIL.CFG`). Before unit selection, it lists available volumes as **unit number + volume name** and prompts for a valid unit in range. Then choose **upload vs download** and enter **remote filename** (e.g. **`image.po`**). **Upload** sends the contents of the selected volume to the server; **download** writes the server file into that volume. Behaviour matches the firmware’s TFTP transfer (same as **`FLASHSOAK`**’s TFTP steps). Large transfers can take several minutes; status ticks appear on **line 21**.

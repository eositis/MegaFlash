# WGET65V — verbose `wget65` (ip65 fork)

Fork of [cc65/ip65 `apps/wget65.c`](https://github.com/cc65/ip65/tree/main/apps) with **on-screen** logging of the Uthernet II / W5100 bring-up path:

- **Step A:** `ip65_init()` (includes stock `w5100.s` RTR XOR probe)
- **Step B:** `w5100_init()` (port mapping + virtual DNS offload hint via PTIMER)
- **Step C:** `dhcp_init()` when DNS offload is off
- **Step D:** `w5100_config()`

After each major step, the program prints a **register snapshot**: Mode register read, RTR0/RTR1 at `$0017/$0018` with the same **XOR chain** ip65 uses (`$D7 ^ RTR0 ^ RTR1` should be `$00` when the probe passes), plus RMSR and PTIMER.

Upstream **ip65** sources are **not** modified in this repo (policy: stock drivers). Only this directory adds the forked client and `wget65_verbose_regs.c`.

**Slot:** **`WGET65V_ETHER_SLOT`** is **4** (MegaFlash Uthernet II at **`$C0C4`–`$C0C7`**). This fork does **not** read **`ethernet.slot`**; stock **`wget65`** still can.

On **Apple II**, **`main()`** calls **`videomode(VIDEOMODE_80COL)`** (same idea as **`hfs65.c`**) so handshake / register dumps use **80 columns** and do not wrap as badly as in 40-column mode.

## Prerequisites

- [cc65](https://cc65.github.io/) (`cl65` on `PATH`)
- A clone of **ip65** at **`MegaFlash/ip65`** (i.e. `../../ip65` from this directory), **or** set **`IP65_ROOT`** to your checkout:

  ```bash
  git clone https://github.com/cc65/ip65.git ../../ip65
  ```

## Build

```bash
cd tools/wget65-verbose
make
```

This builds **`wget65v.bin`** (gitignored). Override the ip65 path:

```bash
make IP65_ROOT=/path/to/ip65
```

## Flash validation disk

If **`tools/wget65-verbose/wget65v.bin`** exists when you run `tools/flash-validate/build-flashval-disk.sh` (default **`WGET65V_BIN`** = **`../wget65-verbose/wget65v.bin`** from **`flash-validate/`**), the script also adds **`WGET65V`**, **`WGET65V.SYSTEM`** (cc65 loader), **`WGET65V`** (Applesoft launcher), **`WGET65V.SRC`**, and **`WGET65V.DOC`**. If the binary is not built, only **`WGET65V.DOC`** is added with build instructions.

Set **`CC65_HOME`** if `cl65` is not on `PATH` but the cc65 share directory is known (the script uses `cl65 --print-target-path` / `apple2enh/util/loader.system`).

## License

Original `wget65` / `w5100` / `linenoise` sources retain the upstream ip65 licenses. Additions in `wget65_verbose_regs.*` follow the MegaFlash project’s contribution terms.

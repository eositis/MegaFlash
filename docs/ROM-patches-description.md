# MegaFlash ROM Patches – Description

The **firmware** folder contains **Apple IIc / IIc+ system ROM patches**. The IIc has no slot ROM socket; $C100–$CFFF is internal ROM only. MegaFlash therefore **replaces** the built-in “Memory Expansion” (Slinky) firmware in **virtual slot 4** by patching the original ROM and merging in new code. The result is `iic.bin` (IIc) or `iicplus.bin` (IIc+), which is burned or loaded into the machine in place of the stock ROM.

---

## Build Process (Two Steps)

1. **Step 1 – Generate patch fragments**  
   CA65 assembles the sources; LD65 links them into **segment binaries** (`b0_c400.bin`, `b1_c580.bin`, `b0_fb19.bin`, etc.) at fixed ROM addresses. The layout is defined in `iic.cfg` / `iicplus.cfg`.

2. **Step 2 – Merge into ROM**  
   CL65 runs with `merge_iic.cfg` / `merge_iicp.cfg` and `merge_iic.s` / `merge_iicp.s`. Those files first `.incbin` the **original** ROM (`rom4.bin` / `rom5.bin`), then overlay the segment binaries at the correct offsets. Output: **`iic.bin`** or **`iicplus.bin`**.

Segment names follow the ROM4X style: **B0_xxxx** = bank 0, **B1_xxxx** = bank 1; **xxxx** is the address (e.g. `B0_C400` = $C400 in bank 0).

---

## Source Modules and What They Patch

| Module | Segments / addresses | Purpose |
|--------|----------------------|--------|
| **slotrom.s** | `SLOTAREA` $C400–$C4FF (bank 0) | **Slot 4 ROM** at $Cn00–$CnFF: signature bytes, SmartPort ID ($00 = no SmartPort, $01 = SmartPort), entry stubs. Boot code is copied from here to RAM. Slot scratch (e.g. `numbanks`, `pwrup`, `toshowbootmenu`, `fpuenabled`) lives in screen holes ($478+n, $4F8+n, etc.). |
| **smartport.s** | `ROM1` → FREEAREA1 (e.g. $D800+) | **SmartPort protocol**: parameter list, status, dispatch to device. Implements the 7-byte SmartPort call interface and calls into the MegaFlash driver (getstatus, readblock, writeblock, etc.). |
| **dispatch.s** | `DISPATCH` ($D800+), `SLXEQHOOK` ($C755), `IOROM` ($C580+) | **Bridge from main ROM to slot 4 code.** Original ROM calls **slxeq** at $C752 to “execute” slot 4; the **hook at $C755** jumps to `slxeqx`, which saves state, switches LC to ROM, calls **dispatch** (which routes to coldstart, clock driver, load cpanel, copy boot menu, etc.), then restores state. So any “call slot 4” from the ROM goes through this dispatcher. |
| **megaflash.s** | `ROM2` / `ROM3` / `ROM4` (e.g. $DC00, $DF00, $D6CE) | **MegaFlash device driver**: SmartPort handlers (getdevstatus, getunitstatus, readblock, writeblock), cold start init, ProDOS clock driver, load Control Panel, write block size to VDH, get DIB/DSB. Talks to the Pico via $C0C0–$C0C3 (command, param, data). |
| **patches.s** | Multiple small segments in bank 0/1 | **System patches:** |
| | **B0_FB19** ($FB19) | **Cold start hook.** Replaces `JMP ($0000)` with load of MODE_INIT and `JMP coldstart2`. `coldstart2` runs the driver’s cold start (slxeq → coldstartinit) then `JMP ($00)` so the rest of boot continues as normal. |
| | **B0_FAC8** ($FAC8) | **Boot menu entry.** When only Closed Apple is pressed at reset, sets `toshowbootmenu` and jumps to power-up so `coldstartinit` can show the boot menu. |
| | **B0_C1DB** ($C1DB) | **Applesoft / Slinky message area** reused for optional code (e.g. IIc+ print-speed fix). |
| | **B0_F315** ($F315) | **ONERR GOTO fix.** Changes `JMP $D7D2` to `JMP $F328` so the stack is reset before the ONERR handler; avoids crash with certain ONERR GOTO loops. |
| | **B0_E112** ($E112) | **INT fix.** Replaces two bytes so Applesoft uses the correct constant for –32768 in integer conversion (fixes “Illegal Quantity” for `A% = A` when A = –32768). |
| | **B0_C7FC / B1_C7FF** | **ROM bank switch for FPU.** When Applesoft calls a math routine, these patches switch to the bank that contains the FPU hook so the 6502 can JSR into the FPU stub. |
| | **B0_E7C6 … B0_ED36** | **Applesoft math hooks (FADD, FMUL, FDIV, FSIN, FCOS, FTAN, FATN, FEXP, FLOG, FSQR, FOUT).** Each original instruction is replaced with a short stub (e.g. `JSR fadd` + NOP). The stub calls **fpu_exec**; if the Pico FPU is enabled, the Pico does the math and returns; otherwise control falls through to the original Applesoft routine. |
| | **IIc+ only** | **B1_C53D** (NOP/CLC) = skip ROM checksum; **B0_C755** = original wait for bell; **B0_FBDD** = bell fix (use orgwait); **B0_FB68** = center title; **B0_DB6C** + **APPLESOFT** = print speed fix (skip wait when A=1). |
| **bootmenu.s** | **B0_FAC8** (entry), code in **ROM1** | **Boot menu:** Entry at $FAC8 detects Ctrl–CA–Reset, sets `toshowbootmenu`. Later, `coldstartinit` copies boot menu code to RAM (`copybm`) and runs it. Menu offers boot from drive, ROM disk position, “Boot to ROM Disk”, load Control Panel, etc. |
| **fpu.s** | **FPU** segment ($DB63+), plus **B0_E7C6 … B0_ED36** stubs | **Applesoft FPU stubs:** Each Applesoft math vector is patched to JSR into a small routine that calls **fpu_exec** with a command (CMD_FADD, CMD_FMUL, …). `fpu_exec` sends the command and operands to the Pico; on success it returns the result; on error it jumps to the Applesoft error handler. Uses zero page and the slot scratch area; fits in the small ROM gaps. |
| **accel.s** | **IIc+ only:** MIG/accelerator and bell/print patches | **IIc+ accelerator and fixes:** Uses MIG RAM at $CE00 for state. Implements accelerator control (speed, lock/unlock), power-up speed selection, and provides **orgwait** (original wait) used by the bell and print-speed patches so 4 MHz isn’t slowed by the normal wait. |

---

## Memory Map (Conceptual)

- **Bank 0:** Main ROM; patches at $C400 (slot ROM), $C1DB, $E112, $E7C6–$ED36 (math), $F315, $FAC8, $FB19, and (IIc+) $C755, $C7FC, $FB68, $FBDD, $DB6C.
- **Bank 1:** Slot 4 “aux” code: $C580 (I/O / slxeq implementation), $C755 (slxeq hook), $C7FF (FPU bank switch), $D800+ (dispatch, SmartPort, MegaFlash driver, boot menu, FPU), and (IIc+) accelerator/bell/print related code.
- **Zero page:** ZPSCRATCH ($3A–$3F) and ZEROPAGE ($48–$4E) for SmartPort and driver; saved/restored around slxeq so ProDOS and other code aren’t broken.

---

## Configuration (buildflags.inc)

- **DEBUG** – Extra serial output for SmartPort/driver (default FALSE).  
- **ONERR_FIX**, **INT_FIX** – Applesoft bug fixes (default TRUE).  
- **PRINTSPEED_FIX** – IIc+ print speed at 4 MHz (default TRUE).  
- **FPUSUPPORT** – Include FPU hooks and stubs (default TRUE).  
- **RESTOREZPSCRATCH** – Restore $3A–$3F after driver (default TRUE).  
- **BOOTANY** – Boot any drive with IN#4 (default FALSE; targets ProDOS 2.5).

---

## Summary

The ROM patches **replace slot 4’s firmware** with MegaFlash (SmartPort + driver), add a **cold start and boot menu** path, **route all “slot 4” calls** through the dispatcher and IOROM, and **optionally fix Applesoft bugs** and **hook math to the Pico FPU**. IIc+ builds add accelerator handling and display/sound/print fixes. The merge step overwrites the original ROM at the segment addresses so that a single ROM image (`iic.bin` / `iicplus.bin`) contains both the original Apple behaviour and MegaFlash.

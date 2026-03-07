# Slot 1 (Serial Port) in iic.bin

This note describes how **slot 1** works in the MegaFlash-patched Apple IIc ROM image **iic.bin** (and the firmware that builds it).

---

## What “slot 1” is on the Apple IIc

On the IIc, **slot 1** is the **built-in Serial Port 1**. It is not a removable card; the machine has fixed “virtual” slots. From the firmware README:

- **Slot 1** – Serial Port 1  
- **Slot 2** – Serial Port 2  
- **Slot 3** – 80 Column Display  
- **Slot 4** – Apple Memory Expansion Card (Slinky) ← replaced by MegaFlash  
- Slot 5 – 3.5" floppy / external Smartport  
- Slot 6 – 5.25" floppy  
- Slot 7 – Mouse  

So in **iic.bin**, “slot 1 functionality” is the **original Apple IIc Serial Port 1 behaviour**, plus the small ways MegaFlash uses that hardware.

---

## Where slot 1 lives (hardware, not in the ROM file)

Serial Port 1 is the **ACIA** (Asynchronous Communications Interface Adapter). Its I/O addresses are defined in **firmware/defines.inc**:

| Address | Name          | Role                |
|---------|---------------|---------------------|
| **$C098** | acia1data    | Data register       |
| **$C099** | acia1status  | Status register     |
| **$C09A** | acia1cmd     | Command register    |
| **$C09B** | acia1ctrl    | Control register    |

These are **hardware registers**. They are not stored inside **iic.bin**; the ROM only contains **code** that reads and writes these addresses. The ROM image is 32 KB (both banks); the code that drives Serial Port 1 is part of the original Apple ROM that is **not** replaced by MegaFlash.

---

## What MegaFlash does and does not change for slot 1

MegaFlash **does not replace or reimplement** the core Serial Port 1 driver. The only places the firmware **touches** slot 1 (ACIA) are:

### 1. Debug build only (megaflash.s)

When **DEBUG = TRUE** in **firmware/buildflags.inc**:

- **Cold start** initializes ACIA 1 for debug output (e.g. 19200 baud, no interrupt).
- A **`print`** routine sends characters to **acia1data** ($C098) and waits on **acia1status** ($C099) so the transmitter is not busy. If bits 5 and 6 of the status register are set (e.g. MAME without the bit-banger option), the routine skips printing so the code does not hang.

So in a **DEBUG** build, slot 1 is used as a **serial debug console**; in release builds it is normal Serial Port 1.

### 2. IIc+ only – boot menu timing (bootmenu.s)

In the boot menu’s **`getkey`** loop (waiting for a keypress), there is:

```asm
lda acia1cmd    ;Touch slot 1 ($C09A) to slow down on IIC+
```

This **reads $C09A** so that on the IIc+, the accelerator drops to 1 MHz when I/O is accessed. The read is only for **timing** (slowing the CPU in the menu); it is not used for serial data.

---

## Summary

| Aspect | In iic.bin / firmware |
|--------|------------------------|
| **Slot 1 driver** | **Original Apple IIc code**; not replaced by MegaFlash. |
| **I/O addresses** | Serial Port 1 at **$C098–$C09B** (ACIA 1). |
| **MegaFlash use** | (1) **DEBUG** build: init + `print` to ACIA for debug output. (2) **IIc+** only: one read of **$C09A** in the boot menu to force 1 MHz. |
| **Slot 1 “ROM”** | No separate “slot 1 card ROM” in the IIc; serial support is in the main ROM. |

So **slot 1 functionality** in **iic.bin** is the standard Apple IIc Serial Port 1, with optional debug use and an IIc+ timing touch only as above.

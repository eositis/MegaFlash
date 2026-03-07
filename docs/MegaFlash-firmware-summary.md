# MegaFlash Firmware – Concise Summary

**MegaFlash** is an internal storage and I/O card for the **Apple IIc / IIc+**. It plugs into the Memory Expansion slot and is built around a **Raspberry Pi Pico (W)** plus **Winbond NOR flash**, with a **GAL** for address decode. The 65C02 only moves data between its RAM and the Pico; the Pico implements storage, network, and peripherals.

---

## Top-Level Layout

| Directory | Role |
|-----------|------|
| **`pico/`** | Pico firmware (C/C++, Pico SDK). Runs on the RP2040/RP2350: bus loop, flash, RAM/ROM disk, Uthernet II, TFTP/NTP, etc. |
| **`firmware/`** | Apple II **ROM patches** (CC65/CA65). Patched system ROM so the IIc uses “virtual slot 4” as MegaFlash instead of the original Memory Expansion firmware. Builds `iic.bin` / `iicplus.bin`. |
| **`cpanel/`** | **Control panel** program. Runs on the Apple II; binary is stored in Pico flash and loaded via `CMD_LOAD_CPANEL`. UI for config, TFTP, ROM disk, etc. |
| **`common/`** | Shared headers: **command codes** (e.g. `CMD_READBLOCK`, `CMD_TFTPRUN`), error codes, and other constants used by pico, cpanel, and firmware. |
| **`kicad/`** | PCB (schematic, layout). |
| **`gal/`** | GAL logic for address decode. |
| **`docs/`** | Design/usage docs (e.g. Uthernet II on MegaFlash, C0x comparison). |

---

## What the Pico Firmware Does (`pico/`)

- **Dual-core:**  
  - **Core 1:** Time-critical **bus loop**. Listens to the Apple bus (PIO), decodes accesses to **$C0C0–$C0CF** (slot 4 I/O), and either:  
    - **C0x0–C0x3 ($C0C0–$C0C3):** MegaFlash protocol (command, param, data, ID).  
    - **C0x4–C0x7 only ($C0C4–$C0C7):** **Uthernet II** (W5100-style) for TCP/UDP/Wi‑Fi. $C0C8–$C0CF are not Uthernet II.  
  - **Core 0:** Network and housekeeping: NTP, TFTP, WiFi, and coordination (e.g. IPC, abort on Apple reset).

- **Storage and “drives”:**
  - **Flash** (Winbond) → ProDOS volumes (e.g. 4–8 drives, 128–256 MB total) via **SmartPort**-style block read/write.
  - **RAM disk** (~400 KB) as a ProDOS device.
  - **ROM disk** (recovery / boot) stored in Pico flash; can be first or last in the unit list.
  - **Slinky** emulation (256 KB) on Pico 2; not on RP2040 due to RAM.

- **Command handling:**  
  Apple sends a **command byte** to **$C0C0** and uses **$C0C1** (param) and **$C0C2** (data) for arguments/blocks. `cmdhandler.c` implements the command set from `common/defines.h`: e.g. `CMD_READBLOCK`, `CMD_WRITEBLOCK`, `CMD_COLDSTART`, `CMD_LOAD_CPANEL`, `CMD_TFTPRUN`, `CMD_ENABLEROMDISK`, FPU ops, RTC, format/erase, settings, etc.

- **Network (Pico W / Pico 2 W):**  
  - **TFTP:** Upload/download ProDOS images over Wi‑Fi.  
  - **NTP:** Set ProDOS time from the network.  
  - **Uthernet II:** W5100 emulation at **$C0C4–$C0C7 only** (lwIP/CYW43); no GPIO slot select; $C0C8–$C0CF are not U2.

- **Other:**  
  - **RTC** (ProDOS clock driver).  
  - **FPU** emulation for Applesoft.  
  - **USB serial** (e.g. XModem transfer).  
  - **Reset handling:** e.g. abort TFTP or flash erase when the Apple resets.

---

## Data Flow (Simplified)

1. Apple **ROM** (patched by `firmware/`) uses virtual **slot 4** as the SmartPort/MegaFlash device.  
2. Apple reads/writes **$C0C0–$C0C3** (command, param, data, ID) to run commands and move 512-byte blocks via the param and data buffers.  
3. **PIO** captures bus cycles and feeds the **bus loop**; it updates the shared `registers` and buffers, and calls **DoCommand()** when the Apple writes a command to $C0C0.  
4. **Cmdhandler** calls into **flash**, **ramdisk**, **romdisk**, **mediaaccess**, **filetransfer**, **userconfig**, etc., and returns status/data through the same registers and buffers.  
5. If the access is to **$C0C4–$C0C7 only**, the bus loop treats it as **Uthernet II** and calls **U2_HandleBusAccess** / **U2_Poll**; $C0C8–$C0CF are not Uthernet II.

So: **MegaFlash firmware = Pico as slot‑4 storage + network**, with ROM patches and cpanel making the IIc talk to it and configure it.

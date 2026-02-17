# Session activity log

This project’s local log. One log per project; stored in the project root.

---

## 2025-02-15

- **Pin 27 deactivated; slot select = default MegaFlash (address only)**: Removed GPIO 27 slot select. `pico/defines.h`: removed `SLOT4_SELECT_GPIO` and `IsSlot4Selected()`; comment now “address decode only, no GPIO slot select”. `pico/busloop.c`: Uthernet II when `addr >= U2_C0X_OFFSET` only (no `IsSlot4Selected()`). `pico/main.c`: removed GPIO 27 init; only `U2_Init()`. `docs/Uthernet-II-emulation-on-MegaFlash.md`: no pin 27, slot select = default bus behaviour.
- **Uthernet II slot 4 only; removed all slot 3 references**: Uthernet uses slot 4 only. `pico/defines.h`: `SLOT3_SELECT_GPIO` → `SLOT4_SELECT_GPIO`, `IsSlot3Selected()` → `IsSlot4Selected()`; comments now $C0C0–$C0C7. `pico/busloop.c`, `pico/main.c`: use slot 4 select. `pico/uthernet2.c` / `uthernet2.h`: comments slot 4 only ($C0C4–$C0C7). `docs/Uthernet-II-emulation-on-MegaFlash.md`: all slot 3 → slot 4, pin 27 = slot 4 device select. `docs/C0x-register-comparison-Uthernet2-MegaFlash.md`: U2 + MegaFlash in slot 4, C0x4–C0x7 vs C0x0–C0x3.
- **Uthernet II at C0x4–C0x7 only**: When slot 4 is selected (GPIO 27), MegaFlash decodes by address: C0x0–C0x3 ($C0C0–$C0C3) = MegaFlash; C0x4–C0x7 ($C0C4–$C0C7) = Uthernet II W5100. `pico/defines.h`: `U2_C0X_OFFSET` (4). `pico/busloop.c`: `U2_HandleBusAccess` only when `IsSlot4Selected() && addr >= U2_C0X_OFFSET`.
- **Build**: Rebuilt Pico (RP2040) and Pico 2 (RP2350) firmware; `pico2_release/megaflash.uf2` and `pico_release/megaflash.uf2` produced successfully.
- **Session log**: Session log updated; rule `.cursor/rules/session-log.mdc` strengthened so the log is updated before ending any turn where code/config or builds were done.

---

## 2025-02-06 (this session)

- **Project overview**: Explained MegaFlash firmware (Apple IIc/IIc+ ROM patches, Slot 4 SmartPort driver, Pico storage). Build: CC65 → ROM fragments → merge into `rom4.bin`/`rom5.bin` → `iic.bin`/`iicplus.bin`.
- **Network service**: Documented TFTP/NTP/WiFi on Pico W (Core 0), command flow Apple → Core 1 → IPC → Core 0, Control Panel TFTP UI and CMD_TFTPRUN/CMD_TFTPSTATUS.
- **ROM disk always available**: Default ROM disk enabled in `pico/romdisk.c`; removed `DisableRomdisk()` from `DoAppleColdStart()` in `pico/cmdhandler.c` and from Control Panel startup in `cpanel/main.c`.
- **ROM disk last unless “Boot to ROM Disk”**: Added ROM disk position (first vs last). `pico/romdisk.c`/`.h`: `GetRomdiskFirst()`/`SetRomdiskFirst()`. `pico/mediaaccess.c`: `TranslateUnitNum` and `GetRamdiskUnitNum()` support ROM-first vs ROM-last. `CMD_ENABLEROMDISK` takes param 0 = last, 1 = first. Cold start sets ROM last. Boot menu “3) Boot ROM Disk” sends param 1. Control Panel: `EnableRomdiskAtLast()` on startup; new menu “Boot to ROM Disk” calls `BootToRomdisk()` (param 1 + reboot). New asm: `EnableRomdiskAtLast`, `BootToRomdisk` in `cpanel/asm-megaflash.s` and `asm.h`.
- **Pico build (macOS)**: Diagnosed missing tools (`PICO_SDK_PATH`, cmake, arm-none-eabi-gcc). Fixed `ranlib` “has no symbols” by forcing ARM `ar`/`ranlib` in `pico/cmakeall.sh`. Addressed `nosys.specs` error (incomplete/alternate toolchain); updated `cmakeall.sh` to pass full paths for C/CXX compiler, AR, RANLIB so CMake caches correct toolchain. Added `ARM_TOOLCHAIN_PATH` and fallback to `/Applications/ArmGNUToolchain/*/arm-none-eabi/bin` so the correct toolchain is used even when PATH has `/usr/local/bin` first.
- **Session log**: Session log moved to project root `SESSION_LOG.md`; rule updated so each project keeps its own log in its local directory.

---

*Append new entries above this line, with date and brief description of changes/commands/decisions.*

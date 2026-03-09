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

## 2026-03-05

- **UDP/TFTP performance documented:** Added §13 to `docs/Implementation-notes-and-reasoning.md`: symptom (slow TFTP, high error rate), root cause (50 ms HEARTBEAT_PERIOD, blocking flash erase), fix (10 ms period), summary and refs. Updated chronology and §11 summary table.
- **UDP task loop period 50 ms → 10 ms:** `pico/udptask.h` HEARTBEAT_PERIOD changed to 10 ms (was 50 ms) to improve TFTP responsiveness and reduce timeouts/retries. Build produced V1.1.10-eo (0x000e); release files in `_releases/V1.1.10-eo/`.
- **Build V1.1.9-eo and release notes:** Ran `pico/cmakeall.sh`; version bumped to V1.1.9-eo (0x000d). Added `pico/_releases/V1.1.9-eo/CHANGELOG.md` (ROM Disk enable/disable, nDEVSEL pull disabled, ReconfigRomdisk, layout fix).
- **Storage device activation: ROM Disk enable/disable:** Added ROM Disk line to Drives Enable page (cpanel). New config bit ROMDISKFLAG in configbyte1 (1=show ROM disk, 0=hide). UI: row “R  ROM Disk  --  [ ]”, key R toggles; Enter applies and sends EnableRomdiskAtLast() or DisableRomdisk() to Pico. asm: DisableRomdisk() (CMD_DISABLEROMDISK). Pico: ReconfigRomdisk() in Reconfig() applies ROMDISKFLAG; DoAppleColdStart() no longer forces EnableRomdisk (config controls it). Default DEFCFGBYTE1 includes ROMDISKFLAG so new configs show ROM disk.
- **nDEVSEL pull disabled:** `pico/a2bus_rp2040.pio` and `pico/a2bus_rp2350.pio`: replaced `gpio_pull_up(nDEVSEL_GPIO)` with `gpio_set_pulls(nDEVSEL_GPIO, false, false)` so nDEVSEL has no internal pull (driven by Apple bus).
- **Pico build requirements doc:** Added `pico/BUILD-REQUIREMENTS.md` documenting SDK, ARM toolchain, CMake, Control Panel and romdisk prerequisites, build script behaviour, and minimal steps to reproduce the build on another machine. Updated `pico/README.md` to point to it. Added **§1.1 Pico SDK add-ons**: table of required submodules (tinyusb, cyw43-driver, lwip, mbedtls, btstack) and why each is needed for pico_w/pico2_w.
- **cmakeall.sh uses PICO_SDK_PATH:** Script now uses `PICO_SDK_PATH` from the environment when set, with a fallback default; exits with a clear error if the SDK path is missing so another machine can set only the env var.
- **Build and release changelog:** Ran `pico/cmakeall.sh`; build produced V1.1.8-eo (0x000c). Added `pico/_releases/V1.1.8-eo/CHANGELOG.md` summarizing concurrent C0xx ranges, U2 C0C4–C0C7 restriction, U2 read-back fix, C0C4 LED, build/release behaviour, and doc/GPIO notes.

---

## 2025-03-02

- **A2/A3 pulldowns disabled**: `pico/a2bus_rp2040.pio` and `pico/a2bus_rp2350.pio`: `gpio_set_pulls(A2ABUS_BASE+2/3, false, true)` → `(false, false)` so A2 and A3 have no pull-ups or pull-downs (driven by Apple bus).
- **Pico firmware rebuilt**: Updated `pico/cmakeall.sh` to pass `-DPICO_SDK_PATH=/Users/eositis/pico-sdk`, reconfigured all CMake builds, and built release UF2 images: `pico_release/megaflash.uf2` (Pico W) and `pico2_release/megaflash.uf2` (Pico 2 W).
- **Build script version bump**: `pico/cmakeall.sh` now increments version and date on each run: reads `FIRMWAREVER`/`FIRMWAREVERSTR` from `defines.h`, bumps patch and hex version, appends `-eo` to string, sets build date, updates `defines.h` and appends a comment line. Fixed hex parsing to use `awk '{print $3}'`. Built firmware at V1.1.6-eo (0x000a).
- **Release output by version**: `pico/cmakeall.sh` builds release targets then copies `megaflash.uf2` from `pico_release` and `pico2_release` into `_releases/<version>/` as `megaflash-pico.uf2` and `megaflash-pico2.uf2` (e.g. `_releases/V1.1.7-eo/`).
- **Uthernet II read-back fix**: $C0C4–$C0C7 reads were returning 0 because U2 read data was merged into PIO chunk 0 ($C0C0–$C0C3). C0C4–C0C7 are served by chunk 1 (SM1). `pico/busloop.c`: U2 read path now updates `registers.i32[1]` and calls `UpdateMegaFlashRegisters(1, merged)` so Mode Register read (e.g. after C0C4:80, C0C4:03) returns 0x03.
- **Implementation reasoning doc**: Added `docs/Implementation-notes-and-reasoning.md` documenting root-cause analysis, design decisions, and references for U2 address/chunk fix, A2/A3 pulldowns, build script (PICO_SDK_PATH, version bump, release folder), and debug mode.
- **C0C4 diagnostic LED**: In `pico/busloop.c`, any access to $C0C4 (U2 Mode Register) turns on the activity LED (ACT_LED_PIN) for 1 second; LED-off is handled non-blocking via `time_reached()` so the bus loop is not blocked.
- **nDEVSEL pull-up disabled**: `pico/a2bus_rp2040.pio` and `pico/a2bus_rp2350.pio`: `gpio_pull_up(nDEVSEL_GPIO)` → `gpio_set_pulls(nDEVSEL_GPIO, false, false)` so nDEVSEL has no internal pull; driven by Apple bus.
- **C0C4 not seen by firmware**: Documented in `docs/Implementation-notes-and-reasoning.md` (§2b): PIO only pushes a cycle when **nDEVSEL (GPIO 20) goes low**; if nDEVSEL doesn’t go low (wrong slot, or floating after pull-up was removed), the CPU never sees the access. Re-enabled **nDEVSEL pull-up** in both PIO files so the line is held high when not selected and the PIO sees a clean low when the card is selected.
- **Implementation notes expanded**: `docs/Implementation-notes-and-reasoning.md` updated with chronology, version-bump script bugs (grep/tr), GPIO pull summary (§8), C0C4 diagnostic LED (§9), reverted nDEVSEL-invert attempt (§10), and open issue “C0C4 not seen” with next steps (§12). Clarified RP2040/RP2350 data path in §2b.
- **Document-thinking rule**: Added `.cursor/rules/document-thinking.mdc` so the agent always records reasoning in SESSION_LOG (brief) and in `docs/Implementation-notes-and-reasoning.md` (root cause, dead ends, takeaways) for future reference and learning.
- **U2 bus loop (colleague’s approach)**: Replaced merge-into-chunk logic with shadow-register approach in `pico/busloop.c`: on U2 read set `registers.r[addr] = u2_read_byte` then `UpdateMegaFlashRegisters(1, registers.i32[1])`; condition `addr >= U2_C0X_OFFSET` (no U2_C0X_LAST in snippet). C0C4 diagnostic LED kept.
- **Concurrent C0xx ranges**: Documented in `busloop.c` and `docs/Implementation-notes-and-reasoning.md` (§1b): C0C0–C0C3 (MegaFlash), C0C4–C0C7 (U2), and C0C8–C0CF (future ACIA) are concurrently active; decode by address only. U2 restricted to 4–7 (`addr <= U2_C0X_LAST`) so C0C8–C0CF remain for ACIA.

---

*Append new entries above this line, with date and brief description of changes/commands/decisions.*

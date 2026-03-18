# Session activity log

This project’s local log. One log per project; stored in the project root.

---

## 2026-03-17

- **Version 1.2.0 / end of 1.1.x:** Set firmware to V1.2.0-eo (0x0020); 1.1.23 is the last 1.1.x build. 1.2 series will focus on Uthernet II emulation, com port, and imagewriter emulation. Updated `pico/defines.h` (version + comment), `pico/CHANGELOG-NEXT.md` (1.2 series intro).
- **TFTP hostname field wrong default (Pico IP / “transferred successfully”):** First time TFTP page opened showed Pico’s IP; after a successful job, field showed status text. Root cause: shared data buffer; PIO was not updated with new DATAREG immediately after CMD_TFTPGETLASTSERVER, so Apple’s first read saw stale data. Fix: in `busloop.c` call `UpdateMegaFlashRegisters(0, registers.i32[0])` right after clearing BUSY in CMDREG handler so chunk 0 (including DATAREG) is pushed before next bus cycle. Defensive: in `cpanel/tftp.c` after `CopyStringFromDataBuffer`, if string looks like status text (e.g. “Successfully”, “Completed”), clear to blank. §7g in Implementation notes.
- **TFTP breaks when debug disabled – lwIP tied to NDEBUG:** Disabling debug (NDEBUG) was changing lwIP: in `lwipopts.h`, LWIP_DEBUG/LWIP_STATS/LWIP_STATS_DISPLAY were set only when `#ifndef NDEBUG`, so release built a different lwIP and TFTP broke. Fixed by setting LWIP_DEBUG, LWIP_STATS, LWIP_STATS_DISPLAY to 0 always so Debug and Release use the same lwIP; only our logging is gated by NDEBUG. Reverted extra cyw43_arch_poll() added for wrong fix. §7f in Implementation notes. `pico/lwipopts.h`, `pico/udptask.cpp`.
- **Release = same codegen as Debug, only NDEBUG:** Debug worked, Release stalled; difference is -O3 (Release) vs -Og (Debug), not just UART. Set CMAKE_C_FLAGS_RELEASE and CMAKE_CXX_FLAGS_RELEASE to "-g -Og -DNDEBUG" in CMakeLists.txt so Release only disables assert/UART; codegen matches Debug. §7e in Implementation notes. Rebuilt pico_release with new flags.
- **Debug build for TFTP stall diagnosis:** Built pico_debug and pico2_debug; copied UF2s to `pico/_releases/debug-build/` (megaflash-pico.uf2, megaflash-pico2.uf2) for testing with UART/serial debug output.
- **TFTP upload stall after ~14 blocks:** Deferred building the next TFTP TX data packet to the start of the event loop (`OnBeforeWait()`) instead of inside the UDP ACK handler, so flash/ReadBlock (SPI + mutex) no longer runs in the receive path. Avoids stall/lockup requiring hard power-off. Added `OnBeforeWait()` hook in `udptask`, deferred-build state and override in `tftptxtask`. §7d in Implementation notes. `pico/udptask.h`, `pico/udptask.cpp`, `pico/tftptxtask.h`, `pico/tftptxtask.cpp`.
- **Release build:** Ran `pico/cmakeall.sh` → version V1.1.21-eo (0x0019), cpanel built, pico_release and pico2_release built, UF2s and CHANGELOG copied to `pico/_releases/V1.1.21-eo/`.
- **Why Release bug was missed / prevention:** Documented in `docs/Implementation-notes-and-reasoning.md` §7b (why missed: Debug-only testing, intentional Release branching looked like design, no Release test requirement, timing-dependent failure). Added prevention: test Release before release, document checklist, minimize critical Debug/Release differences, CI for both builds. Added §7c Pre-release verification checklist (boot, network NTP/TFTP/WiFi test, bus/Apple). Updated summary table (§11).
- **Release 1.1.20 network stack fix:** In Release we only started Core1 and ran core0Loop() when `appleConnected` was true; if `IsAppleConnected()` was false at boot (timing/PHI0), the network loop never ran and “nothing worked.” Now: always launch Core1 (match Debug); use `CheckPicoW()` for Core0 so on Pico W we always run core0Loop() (NTP/TFTP/WiFi) regardless of appleConnected. Documented in `docs/Implementation-notes-and-reasoning.md` §7b. `pico/main.c`.

---

## 2026-03-09

- **U2 read-after-write fix:** PIO chunk 1 ($C0C4–$C0C7) was only updated on U2 reads; writes never called UpdateMegaFlashRegisters(1, ...). SM 1 output stale 0x00 on read-after-write. Now: on U2 write to Mode Reg / Addr High / Addr Low, set registers.r[addr]=data and always call UpdateMegaFlashRegisters(1, ...) for both reads and writes. Data Port (addr 7) readback comes from memory; still updated on read only. `pico/busloop.c`.
- **Toolchain fix in cmakeall.sh:** Pass CMAKE_OBJDUMP and CMAKE_OBJCOPY from TOOLCHAIN_BIN so build no longer uses /usr/local/bin/arm-none-eabi-objdump. Export PATH with toolchain bin first; add PICO_TOOLCHAIN_PATH for SDK compiler lookup. `pico/cmakeall.sh`.
- **TFTP receive fix when reusing WiFi:** When ConnectWifi returns early (already LINK_UP), added ~100 ms warmup: 100× `cyw43_arch_poll()` + 1 ms sleep before returning. Stabilizes lwIP/CYW43 so new UDP PCB can receive (pico-sdk #915). TFTP no longer stuck at "Requesting Server". `pico/udptask.cpp`, `CHANGELOG-NEXT.md`.
- **Drives Enable checkboxes alignment:** Draw checkboxes inline in PrintDriveListWithCheckboxes (cputc tick/space when printing each "( )") so they align with drive rows. ROM disk checkbox unchanged. `cpanel/ui-misc.c`, `cpanel/drivesenable.c`, `cpanel/ui-misc.h`.
- **cmakeall cpanel step:** cmakeall.sh now builds cpanel first (make -C ../cpanel) before Pico firmware. Decremented version to 0x0012/V1.1.14-eo, then ran build → V1.1.15-eo release with cpanel included. `pico/cmakeall.sh`, `defines.h`.

## 2026-03-10

- **Apple II API reference doc:** Added `docs/MegaFlash-AppleII-API.md` documenting all C0C0 commands, their 65C02 calling sequences, equivalent Applesoft PEEK/POKE usage, and how ROM hooks (FPU, clock, SmartPort) map onto these commands so other languages can bypass ROM if needed.
- **cc65 MegaFlash library:** Added `cc65/megaflash.h` and `cc65/megaflash.c` exposing a small C API for unit enumeration, volume info, block I/O, firmware/time strings, ROM disk control, and WiFi self-test on Apple IIc/IIc+. Documented usage and examples in `docs/cc65-megaflash-lib.md`.
- **A2osX C MegaFlash library:** Added `a2osx/megaflash_a2osx.h` / `.c`, a portable K&R-style C wrapper around the same MegaFlash API for the C compiler used in A2osX. It avoids cc65-specific features and uses simple typedefs (`mf_u8`, `mf_u16`, `mf_u32`). Usage and integration notes for A2osX are documented in `docs/a2osx-megaflash-lib.md`.
- **cc65 FPU bindings:** Extended the cc65 library (`cc65/megaflash.h/.c`) with an MBF-level FPU API: `mf_fpu_args_t`/`mf_fpu_result_t` plus `mf_fpu_op` and wrappers (`mf_fadd`, `mf_fmul`, `mf_fdiv`, `mf_fsin`, etc.). These send the same 13-byte FAC/ARG layout used by Applesoft hooks and read back the 8-byte MBF result, allowing C programs to drive MegaFlash’s FPU without using ROM Applesoft. Documented the layout and usage in `docs/cc65-megaflash-lib.md`.

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

## 2026-03-02

- **ROM disk: single row, exclude from TFTP/Format:** ROM disk was shown twice (in PrintDriveList + custom row) and in TFTP/Format. Added `GetDriveListCount()` to exclude ROM disk when last unit. Drives Enable: use listCount for PrintDriveList, show ROM on separate row; fix checkbox rows to `YPOS+i`. TFTP and Format: use GetDriveListCount. `cpanel/ui-misc.c`, `drivesenable.c`, `tftp.c`, `format.c`.
- **Release notes by default:** cmakeall.sh now always includes CHANGELOG.md in the release dir. If `pico/CHANGELOG-NEXT.md` exists, it is copied with `@VERSION@` replaced; else a stub is created. Added V1.1.14-eo CHANGELOG, created CHANGELOG-NEXT.md template. `pico/cmakeall.sh`, `BUILD-REQUIREMENTS.md`.
- **Pico firmware build:** Ran cpanel make + `pico/cmakeall.sh`; built V1.1.14-eo (0x0012). Release UF2s in `_releases/V1.1.14-eo/`.
- **WiFi kept online between sessions:** Destructor no longer disconnects/deinits WiFi; only tears down UDP PCB and buffers. InitCyw43() uses static s_cyw43Inited guard to skip re-init. ConnectWifi() already returns when LINK_UP. Subsequent TFTP/NTP/TestWifi sessions reuse connection. `pico/udptask.cpp`.
- **Build ID left of clock (ProDOS-safe):** Reverted CMD_GETTIMESTR to original 8-byte format for ProDOS compatibility. Added CMD_GETFIRMWAREVER (0x29) to poll build version separately. cpanel DisplayTime calls both: version at cols 20-31, time at 32-39. Boot menu unchanged (time only). `common/defines.h`, `pico/cmdhandler.c`, `cpanel/asm-megaflash.s`.
- **TFTP revert to original state:** Reverted all TFTP-related changes (buffering, session cleanup, timing). Restored from 9159c7e: `udptask.cpp`, `udptask.h`, `tftprxtask.cpp`, `tftprxtask.h`, `flash.c`, `flash.h`. Restored from a6a3498: `network.cpp`, `tftpstate.c`, `tftpstate.h`. Build verified.
- **Drives Enable ROM Disk row fix:** ROM Disk line was drawn at `YPOS+unitCount+1` (same row as last drive/RAM Disk), so it overwrote drive 5. Fixed: `romdiskRow = YPOS+unitCount+2` so ROM Disk is on the row below the last drive. Aligned checkbox rows with `PrintDriveList` (header + unitCount lines): use `YPOS+1+i` for drive i so checkboxes match the printed drive lines; event-loop toggles updated. `cpanel/drivesenable.c`.
- **Pico firmware build:** Ran `pico/cmakeall.sh`; built V1.1.13-eo (0x0011). Release UF2s in `_releases/V1.1.13-eo/`. Added `CHANGELOG.md` (HEARTBEAT revert, ROM Disk row, cpanel).
- **cpanel build:** Added `Beep()` in asm-megaflash.s (cc65 2.18 lacks beep). `#define beep Beep` in asm.h. Fixed drivesenable C89 (const→static_local). Built cpanel.bin.
- **ROM Disk row in Drives Enable:** ROM Disk line was not visible due to wrong row (unitCount+1 overwrote last drive). Fixed: use YPOS+unitCount+1 for ROM Disk row and YPOS+i for drive checkbox rows so ROM Disk appears on the line after the last drive. Event-loop toggles updated. `cpanel/drivesenable.c`.
- **HEARTBEAT_PERIOD reverted 10→50 ms:** 10 ms loop caused upload timeouts and crashes (lwIP re-entrancy risk). Back to 50 ms in `pico/udptask.h`.
- **TFTP session cleanup after cancel/error:** Added `TFTPResetSessionState()` in `tftpstate.c` to reset taskid, blockTransferred, tsize, retries, error, status to IDLE (keeps unitNum, dir, hostname, filename set by DoTFTPRun). Called at the start of `ExecuteTFTP()` in `network.cpp` so each new TFTP run starts clean and does not reuse the previous session. Fixes job not cleaned up and restart reconnecting to previous session. Build V1.1.12-eo.
- **TFTP RX buffering during flash write:** Incoming TFTP DATA packets are now buffered while the RX task is blocked in `WriteBlockForImageTransfer()` (e.g. during sector erase). `flash.c`: added `flash_set_yield_cb()` and yield every ~1 ms in `WaitUntilBusyClear()` so `cyw43_arch_poll()` runs during erase. `udptask`: virtual `OnUDPPacketReceived()` and `DrainOneQueuedPacket()`; callback enqueues when task consumes. `tftprxtask`: 16-slot queue for DATA payloads, `enqueue_data_packets` flag around flash writes, yield callback registered in `Run()`. Event loop drains one queued packet per iteration. Doc §13c in `Implementation-notes-and-reasoning.md`. Build V1.1.11-eo.
- **USB XMODEM vs TFTP write handling:** Documented in `docs/Implementation-notes-and-reasoning.md` (§13b) why USB xmodem/ymodem completes with no delays while TFTP can be slow: same `WriteBlockForImageTransfer()` and ACK-before-write; difference is synchronous USB loop + USB buffering vs event-loop + no `cyw43_arch_poll()` during blocking flash erase.

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
## 2025-03-02

... (entries above unchanged) ...

- **U2 bus loop (colleague’s approach)**: Replaced merge-into-chunk logic with shadow-register approach in `pico/busloop.c`: on U2 read set `registers.r[addr] = u2_read_byte` then `UpdateMegaFlashRegisters(1, registers.i32[1])`; condition `addr >= U2_C0X_OFFSET` (no U2_C0X_LAST in snippet). C0C4 diagnostic LED kept.
- **Concurrent C0xx ranges**: Documented in `busloop.c` and `docs/Implementation-notes-and-reasoning.md` (§1b): C0C0–C0C3 (MegaFlash), C0C4–C0C7 (U2), and C0C8–C0CF (future ACIA) are concurrently active; decode by address only. U2 restricted to 4–7 (`addr <= U2_C0X_LAST`) so C0C8–C0CF remain for ACIA.

## 2026-03-17

- **Planned network refactor documented:** Extended `docs/Implementation-notes-and-reasoning.md` with §14 describing a future NetworkPump + INetworkSession architecture so NTP, TFTP, TestWifi, and Uthernet II TCP/UDP can share cyw43/lwIP concurrently, and how to abort/reset all sessions cleanly on Apple II reset.
- **NetworkPump skeleton added:** Created `pico/network_pump.h` and `pico/network_pump.cpp` defining a preliminary `NetworkPump` and `INetworkSession` interface (WiFi init/connection helpers, UDP/TCP pcb helpers, stubbed session/timer/abort APIs). Added `network_pump.cpp` to `pico/CMakeLists.txt`. No existing code uses the pump yet, so runtime behaviour is unchanged; this is groundwork for migrating NTP/TFTP/Uthernet II to a shared pump later.
- **TFTP lifecycle moved into manager:** `ExecuteTFTP()` now delegates to `NetworkPump::RunTFTP(...)` so the manager owns TFTP start/finish bookkeeping and reset-abort routing. Added `NetworkPump_RequestAbortAll()` and changed the Apple reset ISR to call it, while keeping the legacy TFTP packet engine intact for now. Built Debug Pico/Pico 2 targets successfully after the change.
- **Apple ROM debug enabled:** Set `firmware/buildflags.inc` `DEBUG` to `TRUE` so the 6502 ROM prints Serial Port 1 debug at 19200 baud. Attempted `make a2c` / `make a2cp`, but the build is currently blocked because `rom4.bin` and `rom5.bin` are not present in the workspace; the ROM build needs those Apple II ROM images in `firmware/` before it can complete.
- **Apple ROM debug built successfully:** Re-ran `make a2c` and `make a2cp` after `rom4.bin` / `rom5.bin` were available in `firmware/`; both completed and produced `iic.bin` and `iicplus.bin` with `DEBUG` enabled.
- **Control Panel TFTP safeguard:** Added a debug safety check in `cpanel/tftp.c` so the TFTP upload screen detects a filename that looks like an IPv4 address, warns the user, and swaps host/file before sending. Built `cpanel.bin` successfully. This was added after debug traces showed the Apple-side TFTP command being populated with a swapped or stale host/file pair.
- **Pico debug rebuild after Control Panel change:** Rebuilt `pico_debug` and `pico2_debug` after updating `cpanel.bin`, so the Pico firmware image now packages the new Control Panel code (`cpanel.s` was rebuilt as part of the Pico target).
- **Control Panel release repackaged into Pico:** Reconfirmed the non-debug Control Panel build path (`make release`) and repackaged the Pico debug targets so the bundled `cpanel.bin` remains the release build while the Pico firmware is rebuilt around it.
- **Versioned release build:** Ran `./cmakeall.sh` to bump the firmware to `V1.1.20-eo` (`0x0018`), rebuild the release firmware, and write `_releases/V1.1.20-eo/CHANGELOG.md`. The generated release notes now include the network stack work: `NetworkPump`/`INetworkSession` skeleton, TFTP lifecycle routing through the manager, the host/file swap safeguard, and the new debug logging details.

---

*Append new entries above this line, with date and brief description of changes/commands/decisions.*

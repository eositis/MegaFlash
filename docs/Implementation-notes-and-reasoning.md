# Implementation Notes and Reasoning

This document records the thinking, root-cause analysis, design decisions, and dead ends from work on the MegaFlash Pico firmware (Uthernet II, build, GPIO, C0C4 visibility). It is intended for future maintainers and for debugging similar issues.

---

## Chronology and context (from session work)

- **Uthernet II**: Confirmed U2 at $C0C4–$C0C7 only; no GPIO slot select. Fixed read-back of Mode Register (chunk 1 vs chunk 0). Added C0C4 diagnostic LED (1 s on any $C0C4 access).
- **GPIO pulls**: A2/A3 pulldowns disabled (bus-driven). nDEVSEL pull-up first disabled at user request, then re-enabled when C0C4 was not seen; with pull-up enabled, C0C4 still not recognized.
- **Build**: PICO_SDK_PATH added to cmakeall.sh; version bump + “-eo” + date on each build; version-bump script fixed (grep uniqueness, strip newlines); release UF2s copied to `_releases/<version>/`.
- **C0C4 not seen**: Logic analyzer shows A0–A4 (and presumably address) at the Pico, but firmware never sees the access (no LED, no response). Conclusion: PIO only pushes a cycle when **nDEVSEL (GPIO 20) goes low**; if it does not go low for that access, the CPU never gets the cycle. Tried inverting nDEVSEL sense (trigger on pin HIGH = DEVSEL active-high); **reverted** at user request. nDEVSEL sense remains active-low.

---

## 1. Uthernet II at $C0C4–$C0C7 (address decode)

**Requirement:** Uthernet II (W5100) should respond to slot 4 addresses $C0C4–$C0C7 only; MegaFlash uses $C0C0–$C0C3.

**Reasoning:**
- The Apple II slot address is the low nibble of the address: $C0C0 → 0, $C0C4 → 4, etc.
- No GPIO slot-select pin is used; decode is by address only (`defines.h`: `U2_C0X_OFFSET` = 4, `U2_C0X_LAST` = 7).
- In `busloop.c`, when `addr >= 4 && addr <= 7` we call `U2_HandleBusAccess`; otherwise we handle MegaFlash (0–3) or other (8–15).
- W5100 indirect access uses four “ports”: C0x4 = Mode Register, C0x5 = Address High, C0x6 = Address Low, C0x7 = Data. The C code uses `busdata & 3` (U2_C0X_MASK) to map 4→0, 5→1, 6→2, 7→3 into W5100 port indices.

**References:** `pico/defines.h`, `pico/busloop.c`, `pico/uthernet2.c`, `pico/w5100_regs.h`.

---

## 1b. C0xx ranges are concurrently active (not mutually exclusive)

**Design:** C0C0–C0C3 (MegaFlash), C0C4–C0C7 (Uthernet II), and the reserved C0C8–C0CB / C0CC–C0CF (future ACIA) are **concurrently available**. The card responds to all of them; which handler runs is determined **only by the address** of the current bus cycle. They are not mutually exclusive (e.g. “slot in U2 mode” vs “slot in MegaFlash mode”) but rather decode by address so that MegaFlash base, U2, and (future) ACIA can all be active at once.

**In code:** The bus loop decodes `addr` (low nibble): 0–3 → MegaFlash (command, param, data, ID); 4–7 → Uthernet II only (`addr >= U2_C0X_OFFSET && addr <= U2_C0X_LAST`); 8–15 left for future ACIA. U2 is restricted to 4–7 so that 8–11 and 12–15 are not consumed by U2 and remain available for ACIA emulation.

**References:** `pico/busloop.c` (address-decode comment and U2 condition).

---

## 2. Uthernet II read-back returning 0 (Mode Register test)

**Symptom:** After writing $80 then $03 to $C0C4 (Mode Register), a read from $C0C4 returned $00 instead of $03, as if the Uthernet II had not received the write.

**Root-cause analysis:**
- The U2 *handler* was correct: `U2_HandleBusAccess` reads/writes `u2_mode_register`; write $80 triggers `u2_reset()`, write $03 sets `u2_mode_register = 0x03`, and a read returns `u2_mode_register`. So the bug was not inside `uthernet2.c`.
- The 6502 read data is supplied by the PIO: when the Apple reads an address, the corresponding PIO state machine drives the data bus with a byte from its “register chunk.”
- In `a2bus.h` and the PIO design, the 16 registers are split into **4 chunks** of 4 bytes, each served by one state machine:
  - **Chunk 0** → $C0C0–$C0C3 (SM0, [A3:A2]=00)
  - **Chunk 1** → $C0C4–$C0C7 (SM1, [A3:A2]=01)
  - Chunk 2 → $C0C8–$C0CB, Chunk 3 → $C0CC–$C0CF
- For a read from $C0C4, the Apple puts address 4 on the bus; the PIO matches [A3:A2]=01, so **state machine 1** responds and outputs the **first byte of chunk 1** (i.e. `registers.i32[1]` byte 0).
- The bus loop was merging the U2 read byte into **chunk 0** (`registers.i32[0]`) and calling `UpdateMegaFlashRegisters(0, merged)`. So SM0’s chunk was updated, but **SM1’s chunk (chunk 1) was never updated** with the U2 result. SM1 therefore kept outputting whatever was last in chunk 1 (e.g. 0), so the 6502 always saw $00 for a read from $C0C4.

**Fix:** For Uthernet II reads (addr 4–7), merge the U2 read byte into **chunk 1** and call `UpdateMegaFlashRegisters(1, merged)` so that the state machine that actually drives the bus for $C0C4–$C0C7 (SM1) receives the correct byte. Byte index within the chunk remains `busdata & 3` (0 for $C0C4, 1 for $C0C5, etc.).

**Takeaway:** When adding a logical “device” at addresses that map to a PIO chunk different from MegaFlash’s (chunk 0), any read data supplied to the 6502 must be written into the **chunk that the PIO uses for that address**, not chunk 0.

**References:** `pico/busloop.c` (U2 read path), `pico/a2bus.h` (chunk comment), `pico/a2bus_rp2040.pio` (SM ID and byte select).

---

## 2b. C0C4 access seen at Pico pins but firmware does not respond (no LED, no read-back)

**Symptom:** Logic analyzer shows the C0C4 access (e.g. address/data) at the Pico, but the firmware never sees it: the C0C4 diagnostic LED does not turn on, and Uthernet II does not respond.

**Root cause:** The bus loop only receives a cycle when the **PIO** captures it and pushes to the FIFO that the CPU reads. On **RP2040**, all four state machines (SM0–SM3) run the same program; when nDEVSEL goes low they all do `in pins` + `push noblock`, so each SM’s TX FIFO gets the bus data. The CPU calls `GetAppleBusBlocking()` which reads **only SM0’s** FIFO (`SM_LISTENER` = 0). So the CPU sees every cycle that the PIO captures. On **RP2350**, a dedicated listener SM pushes every captured cycle to its FIFO, which the CPU reads. In both cases, the PIO only leaves its wait loop and runs `in pins` + `push` when **nDEVSEL (GPIO 20) goes low**. So:

- If **nDEVSEL never goes low** for that access, the PIO never pushes a bus cycle to the CPU. The CPU stays blocked in `GetAppleBusBlocking()` and never runs the U2/LED code.
- So the address/data you see on the logic analyzer (e.g. A0–A3, D0–D7) are not enough; **nDEVSEL must be asserted (low)** for the slot/address the card is in.

**What to check:**

1. **Probe nDEVSEL (GPIO 20)** when you access C0C4. It must go **low** for the duration of the cycle. If it stays high or floats, the Pico will never see the access.
2. **Slot selection:** The Apple II selects a slot when addressing the C0xx page. MegaFlash is in **slot 4**. If the monitor or your test code addresses **slot 0** $C0C4 (or another slot), then **slot 4’s** nDEVSEL is not asserted. So the card in slot 4 never sees the cycle. Ensure you are addressing **slot 4** (e.g. the same slot the rest of MegaFlash uses for $C0C0–$C0C3).
3. **nDEVSEL pull-up:** If nDEVSEL has no pull-up and the slot decode (e.g. GAL) or bus does not drive it when inactive, the line can **float**. A floating input may not cross the logic threshold when the driver does assert low, or may glitch. Re-enabling the **pull-up on nDEVSEL** holds the line high when not selected and gives a clean low when the card is selected; it is recommended unless your hardware explicitly drives nDEVSEL high when inactive.

**Summary:** The CPU only gets cycles that the PIO pushes; the PIO only pushes when nDEVSEL is low. Verify nDEVSEL (GPIO 20) goes low for slot 4 C0C4 accesses, and that you are addressing slot 4. Re-enable nDEVSEL pull-up if the line can float.

---

## 3. A2/A3 GPIO and pulldowns

**Requirement:** Confirm and later change A2/A3: Pico reads A2 from GPIO 8 and A3 from GPIO 9; user requested pulldowns disabled.

**Reasoning:**
- In both `a2bus_rp2040.pio` and `a2bus_rp2350.pio`, `A2ABUS_BASE` = 6, so A0–A3 are GPIO 6–9. Thus A2 = GPIO 8, A3 = GPIO 9.
- The original comment said A2/A3 were pulled down so that “address at $C0C0–$C0C3” could be selected when those lines were floating (e.g. on Rev 1.0/1.1 PCB). With A2/A3 actually driven by the Apple bus, pull-downs can force the lines low when the bus is high-Z and distort the address. Disabling pull-downs avoids that and lets the Apple drive the pins.
- Change: `gpio_set_pulls(A2ABUS_BASE+2, false, true)` → `gpio_set_pulls(..., false, false)` (and same for +3) in both PIO init sections.

**References:** `pico/a2bus_rp2040.pio`, `pico/a2bus_rp2350.pio`.

---

## 4. Build script: PICO_SDK_PATH

**Symptom:** `./cmakeall.sh` failed with “SDK location was not specified” (CMake `pico_sdk_import.cmake`).

**Reasoning:**
- The script was not passing `PICO_SDK_PATH` to CMake. The environment might have it set in an interactive shell, but when the script runs (e.g. from Cursor/IDE), that env may not be set, and CMake’s cache can change (e.g. after toolchain change), causing a re-run where `PICO_SDK_PATH` is empty.
- Making the build reproducible: pass the SDK path explicitly on the command line so the script does not depend on the environment. The path used was `/Users/eositis/pico-sdk` (user’s machine); others can set `PICO_SDK_PATH` or edit the script.

**Change:** Add `-DPICO_SDK_PATH=/Users/eositis/pico-sdk` to every `cmake -B ...` line in `cmakeall.sh`.

**References:** `pico/cmakeall.sh`, `pico/pico_sdk_import.cmake`.

---

## 5. Build script: auto-increment version and “-eo” suffix

**Requirement:** On each build, increment version and date, and append “-eo” to the version string to denote a custom (user) build.

**Reasoning:**
- Version lives in `pico/defines.h`: `FIRMWAREVER` (hex, e.g. 0x0009) and `FIRMWAREVERSTR` (e.g. "V1.1.5"). The script runs at the start of each build, so we need to:
  1. Read current values.
  2. Increment hex version (e.g. 0x0009 → 0x000a) and patch (e.g. 1.1.5 → 1.1.6).
  3. Set string to "V<major>.<minor>.<patch>-eo".
  4. Set build date (e.g. `date +%d-%b-%Y`).
  5. Write back to `defines.h` and append a comment line for the new version/date.
- First attempt parsed the hex with `sed 's/.*0x/0x/;s/[^0-9A-Fa-f].*//'`, which stripped the “x” and left only “0”, so the next version became 0x0001. Correct parsing is to take the third field: `awk '{print $3}'` so we get the full token (e.g. 0x0009).
- **Second bug:** `grep '^#define FIRMWAREVER'` matched both `FIRMWAREVER` and `FIRMWAREVERSTR`, so `CURRENT_HEX` captured two lines (“0x000a” and “"V1.1.6-eo"”), and `$((CURRENT_HEX))` caused “invalid arithmetic operator”. Fix: use `grep '^#define FIRMWAREVER '` and `grep '^#define FIRMWAREVERSTR '` (trailing space) so each macro matches exactly one line.
- **Third bug:** Command substitution leaves a trailing newline; `$((0x000a\n))` is invalid. Fix: pipe through `tr -d '\r\n'` when capturing `CURRENT_HEX` and `CURRENT_STR`.
- If the string already had “-eo”, we strip it before parsing so the patch number is numeric (e.g. 1.1.6 from "V1.1.6-eo").

**References:** `pico/cmakeall.sh`, `pico/defines.h`.

---

## 6. Build script: release outputs in versioned folder

**Requirement:** Put build results in a folder named by the release/version number.

**Reasoning:**
- After a successful build, the useful artifacts are the two UF2 files: `pico_release/megaflash.uf2` (Pico W) and `pico2_release/megaflash.uf2` (Pico 2 W).
- Using the version string already computed for this run (e.g. `NEW_VER="V1.1.6-eo"`) gives a unique, human-readable folder name.
- Flow: run `make` for both release dirs, then `mkdir -p _releases/$NEW_VER`, copy the two UF2s into that folder as `megaflash-pico.uf2` and `megaflash-pico2.uf2`. The script already has `NEW_VER` from the version-bump block, so we only create the folder when `NEW_VER` is set (i.e. when `defines.h` was present and updated).

**References:** `pico/cmakeall.sh`, `pico/_releases/`.

---

## 7. Pico “debug mode” (Debug build)

**Requirement:** Understand what “debug mode” provides when the Pico firmware is built as Debug.

**Reasoning:**
- “Debug mode” is determined by **build type**, not a runtime flag: Debug build = `NDEBUG` not defined; Release = `NDEBUG` defined. So it’s “Debug build” vs “Release build.”
- In Debug, `pico/debug.h` macros (e.g. `DEBUG_PRINTF`) expand to `printf(...)`; in Release they are no-ops. So all debug logging is compiled in only for Debug.
- `main.c`: In Debug, UART stdio is initialized (115200 baud) and the bus loop (Core 1) is **always** started, even if the Apple is not connected, so you can plug in the Apple later and test. In Release, the bus loop is started only when `IsAppleConnected()` is true.
- Startup banner (version, CPU/peri clock, SPI speed, WiFi support, heap) is printed only when Debug macros are active (i.e. Debug build).
- lwIP: In `lwipopts.h`, when `NDEBUG` is not defined, `LWIP_DEBUG` is set to 1 so the network stack can produce extra debug output.
- TinyUSB: Debug build compiles with `CFG_TUSB_DEBUG=1`.

So “Pico debug mode” = Debug build → UART console, all DEBUG_PRINTF-style logging, bus loop always on, optional lwIP/TinyUSB debug. Release → no UART stdio, no debug prints, bus loop only when Apple connected.

**References:** `docs/Debug-mode.md`, `pico/main.c`, `pico/debug.h`, `pico/lwipopts.h`.

---

## 8. GPIO pull state (A0–A3, nDEVSEL, data bus)

| Signal    | GPIO | Pull-up | Pull-down | Notes |
|-----------|------|---------|-----------|--------|
| A0        | 6    | No      | No        | Not explicitly set; default no pull |
| A1        | 7    | No      | No        | Not explicitly set |
| A2        | 8    | No      | No        | Explicitly set (false, false) so bus drives |
| A3        | 9    | No      | No        | Explicitly set (false, false) |
| nDEVSEL   | 20   | **Yes** | No        | Re-enabled so line is held high when not selected |
| Data bus  | 11–18| Yes     | No        | Avoid floating transceiver inputs |

**References:** `pico/a2bus_rp2040.pio`, `pico/a2bus_rp2350.pio` (C SDK init blocks).

---

## 9. C0C4 diagnostic LED

**Purpose:** Confirm whether the firmware ever “sees” a $C0C4 access (U2 Mode Register).

**Implementation:** In `busloop.c`, when `addr == U2_C0X_OFFSET` (4) inside the U2 block, turn on the activity LED (ACT_LED_PIN) and set a 1 s timeout. At the top of the loop, if the timeout has passed, turn the LED off. Uses `pico/time.h` (`make_timeout_time_ms`, `time_reached`) so the bus loop is never blocked.

**Interpretation:** If the LED never turns on when you access C0C4, the bus loop never receives a cycle with addr 4 — i.e. the PIO never pushed that cycle (e.g. nDEVSEL did not go low). If the LED turns on, the CPU is seeing C0C4 and the remaining issue is response/read-back (e.g. chunk, timing).

**References:** `pico/busloop.c`, `pico/defines.h` (ACT_LED_PIN).

---

## 10. Attempted fix: nDEVSEL sense inverted (reverted)

**Idea:** On some boards the “device select” line might be **active-high** (DEVSEL) rather than active-low (nDEVSEL). With pull-up enabled, the line is high when not selected; if the board asserts **high** when selected, we would never see a low and would never trigger.

**Change tried:** In both PIO files, trigger when the pin goes **high** instead of low: RP2040 `jmp PIN, loop` → `jmp !PIN, loop`; RP2350 listener `wait 0` → `wait 1`; and at end of cycle wait for pin low instead of high.

**Outcome:** User asked to revert; reverted. nDEVSEL is again defined as active-low (proceed when pin low, wait for pin high when cycle ends).

**References:** `pico/a2bus_rp2040.pio`, `pico/a2bus_rp2350.pio`.

---

## 11. macOS build: Homebrew toolchain vs official ARM toolchain

**Symptom:** Build fails with “cannot read spec file 'nosys.specs'” at boot_stage2 link, or (after adding nosys.specs) with “stdio.h / assert.h: No such file or directory” when compiling the main firmware.

**Reasoning:**
- Homebrew’s `arm-none-eabi-gcc` is built with `--without-headers`: it does not ship newlib (no `stdio.h`, `assert.h`, or C library headers). The compiler’s `-print-search-dirs` shows “ignoring nonexistent directory .../arm-none-eabi/include”.
- The Pico SDK’s boot_stage2 build uses `--specs=nosys.specs`. Homebrew’s toolchain also does not ship `nosys.specs` in its lib dir, so the link fails unless a spec file is provided.
- **Fix for boot_stage2:** A minimal `nosys.specs` is bundled in `pico/scripts/nosys.specs` (renames `link_gcc_c_sequence`, overrides it to only add empty `--start-group`/`--end-group` so no `-lc`/`-lnosys` are pulled in). Copy it into the toolchain’s spec search path, e.g.  
  `cp pico/scripts/nosys.specs "$(dirname $(arm-none-eabi-gcc -print-file-name=libc.a))/../lib/gcc/arm-none-eabi/$(arm-none-eabi-gcc -dumpversion)/"`  
  (or the path reported by `arm-none-eabi-gcc -print-file-name=nosys.specs`’s install dir). Then boot_stage2 links successfully.
- **Main firmware:** Even with nosys.specs in place, the main app needs standard headers. Homebrew’s gcc has no newlib include path, so compilation fails. **Solution:** Use the [official ARM GNU toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (macOS .pkg), which includes newlib. Set `ARM_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` directory before running `./cmakeall.sh`.
- Using a compiler wrapper (e.g. to rewrite `--specs=nosys.specs` to the bundled file) as `CMAKE_C_COMPILER` causes the SDK to set `CMAKE_FIND_ROOT_PATH` from the wrapper’s directory, so includes are looked for under the project (e.g. `pico/scripts/`) and stdio.h is still not found. So the wrapper is not used in `cmakeall.sh`; the real compiler path is passed, and nosys.specs is installed into the toolchain when using Homebrew (for boot_stage2 only; full build still needs official toolchain for headers).

**References:** `pico/scripts/nosys.specs`, `pico/cmakeall.sh`, `pico/README.md`.

---

## 12. Summary table of code locations

| Topic | Key files | Decision / fix |
|-------|-----------|----------------|
| U2 address range | `defines.h`, `busloop.c` | C0x4–C0x7 only; no GPIO slot select |
| U2 read data path | `busloop.c`, `a2bus.h` | U2 read byte must update **chunk 1** (SM1), not chunk 0 |
| A0–A3, nDEVSEL pulls | `a2bus_rp2040.pio`, `a2bus_rp2350.pio` | A2=GPIO8, A3=GPIO9, no pulls; nDEVSEL pull-up on; data bus pull-up |
| Build SDK path | `cmakeall.sh` | Explicit `-DPICO_SDK_PATH=...` for all cmake invocations |
| Version bump | `cmakeall.sh`, `defines.h` | Grep with trailing space; `awk '{print $3}'`; `tr -d '\r\n'`; string = "Vx.y.z-eo" |
| Release output | `cmakeall.sh` | Build then copy UF2s to `_releases/<NEW_VER>/` |
| Debug behaviour | `main.c`, `debug.h`, `lwipopts.h` | Debug build = UART + logs + bus loop always; Release = no UART, no logs, bus loop only when Apple connected |
| C0C4 diagnostic | `busloop.c` | LED on 1 s on any $C0C4 access; non-blocking |
| nDEVSEL sense | Both PIO files | Active-low (trigger on low); inverted sense was tried and reverted |
| macOS build | `README.md`, `scripts/nosys.specs` | Homebrew gcc has no newlib; use official ARM toolchain + optional copy of nosys.specs into toolchain lib |

---

## 13. Open / unresolved: C0C4 not seen by firmware

**Observed:** With a logic analyzer, A0–A4 (and thus the address) are confirmed at the Pico when C0C4 is accessed. With nDEVSEL pull-up enabled, the Pico still does not recognize the access (no LED, no U2 response).

**Implication:** The PIO only runs `in pins` + `push` when **nDEVSEL (GPIO 20) goes low**. So either:

1. **nDEVSEL never goes low** for this access (e.g. slot decode does not assert it for $C0C4, or only for $C0C0–$C0C3; or the monitor/test addresses a different slot; or the line is not wired/used on this connector), or  
2. **nDEVSEL goes low but something else** prevents the PIO from pushing or the CPU from reading (e.g. timing, FIFO, wrong SM on RP2040).

**Next steps for debugging:**

- **Probe nDEVSEL (GPIO 20)** during a C0C4 access. If it never goes low, the fix is hardware or slot/decode (ensure slot 4 and full $C0C0–$C0CF range assert nDEVSEL). If it does go low, the bug is elsewhere (timing, FIFO, or CPU not draining SM0 on RP2040).
- **Confirm slot:** Ensure the test (monitor or program) is addressing **slot 4** (same as $C0C0–$C0C3 that work for MegaFlash).
- **Hardware docs:** Check whether the IIc memory expansion connector exposes nDEVSEL and for which address range it is asserted (e.g. GAL or schematic).

---

*This document reflects reasoning and changes made during development; it may be extended as further design decisions are documented.*

# Implementation Notes and Reasoning

This document records the thinking, root-cause analysis, design decisions, and dead ends from work on the MegaFlash Pico firmware (Uthernet II, build, GPIO, C0C4 visibility). It is intended for future maintainers and for debugging similar issues.

---

## Chronology and context (from session work)

- **Uthernet II**: Confirmed U2 at $C0C4–$C0C7 only; no GPIO slot select. Fixed read-back of Mode Register (chunk 1 vs chunk 0). Added C0C4 diagnostic LED (1 s on any $C0C4 access).
- **GPIO pulls**: A2/A3 pulldowns disabled (bus-driven). nDEVSEL pull-up first disabled at user request, then re-enabled when C0C4 was not seen; with pull-up enabled, C0C4 still not recognized.
- **Build**: PICO_SDK_PATH added to cmakeall.sh; version bump + “-eo” + date on each build; version-bump script fixed (grep uniqueness, strip newlines); release UF2s copied to `_releases/<version>/`.
- **C0C4 not seen**: Logic analyzer shows A0–A4 (and presumably address) at the Pico, but firmware never sees the access (no LED, no response). Conclusion: PIO only pushes a cycle when **nDEVSEL (GPIO 20) goes low**; if it does not go low for that access, the CPU never gets the cycle. Tried inverting nDEVSEL sense (trigger on pin HIGH = DEVSEL active-high); **reverted** at user request. nDEVSEL sense remains active-low.
- **TFTP/UDP performance**: TFTP was slow and had high error rate. Root cause: 50 ms HEARTBEAT_PERIOD in udptask added up to 50 ms latency per UDP packet, and blocking flash sector erase (~220–250 ms every 16 blocks) stalled the event loop. No documented reason for 50 ms. HEARTBEAT_PERIOD reduced to 10 ms to improve TFTP (and NTP/Test WiFi) responsiveness (see §13).

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

## 11. Summary table of code locations

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
| TFTP/UDP performance | `udptask.h`, `udptask.cpp` | HEARTBEAT_PERIOD 50→10 ms; 50 ms added latency per packet; blocking flash erase also stalls loop (see §13) |

---

## 12. Open / unresolved: C0C4 not seen by firmware

**Observed:** With a logic analyzer, A0–A4 (and thus the address) are confirmed at the Pico when C0C4 is accessed. With nDEVSEL pull-up enabled, the Pico still does not recognize the access (no LED, no U2 response).

**Implication:** The PIO only runs `in pins` + `push` when **nDEVSEL (GPIO 20) goes low**. So either:

1. **nDEVSEL never goes low** for this access (e.g. slot decode does not assert it for $C0C4, or only for $C0C0–$C0C3; or the monitor/test addresses a different slot; or the line is not wired/used on this connector), or  
2. **nDEVSEL goes low but something else** prevents the PIO from pushing or the CPU from reading (e.g. timing, FIFO, wrong SM on RP2040).

**Next steps for debugging:**

- **Probe nDEVSEL (GPIO 20)** during a C0C4 access. If it never goes low, the fix is hardware or slot/decode (ensure slot 4 and full $C0C0–$C0CF range assert nDEVSEL). If it does go low, the bug is elsewhere (timing, FIFO, or CPU not draining SM0 on RP2040).
- **Confirm slot:** Ensure the test (monitor or program) is addressing **slot 4** (same as $C0C0–$C0C3 that work for MegaFlash).
- **Hardware docs:** Check whether the IIc memory expansion connector exposes nDEVSEL and for which address range it is asserted (e.g. GAL or schematic).

---

## 13. TFTP / UDP performance: slow transfer and high error rate

**Symptom:** TFTP transfers are rather slow in practice and have a fairly high error rate (timeouts, retries, failed or flaky transfers).

**Is it Pico network hardware?** The Pico W’s CYW43439 can achieve on the order of several Mbit/s in good conditions. TFTP is stop-and-wait (one DATA, one ACK per block), so even at 512-byte blocks we use only a small fraction of that capacity. The observed slowness and errors are dominated by **software and timing**, not by raw WiFi capability.

**Root-cause analysis:**

1. **50 ms event-loop period (`HEARTBEAT_PERIOD`)**  
   The UDP task event loop (used by TFTP, NTP, and Test WiFi) runs with `cyw43_arch_wait_for_work_until(nextRun, ...)` where `nextRun` is **now + 50 ms**. Incoming UDP is only processed on the **next** iteration of the loop. So from “packet arrives” to “we call `EvtUDPReceived`” we can add **up to 50 ms** every time. For TFTP: server sends DATA → we may wait up to 50 ms before we even see it → then we ACK. That inflates round-trip time per block, lowers throughput, and makes server timeouts and retries much more likely (many servers use 1–3 s; we often cannot respond quickly).

2. **Blocking flash writes (especially sector erase)**  
   On download, every received block is written with `WriteBlockForImageTransfer()`. For flash, every **16 blocks** we call `tsEraseSector64k()` (comment in `flash.c`: “Actual Test: 220–250 ms”). While that runs, the TFTP event loop does **not** run: no `cyw43_arch_poll()`, no handling of the next DATA, no ACK sent. The server can wait 220–250 ms (or more) for an ACK, then timeout and retransmit, leading to retries, duplicate blocks, and perceived “high error rate.”

3. **No documented reason for 50 ms**  
   The code does not state why 50 ms was chosen. The only comment is that the loop runs every `HEARTBEAT_PERIOD` to check the abort flag. Abort only needs to be checked “every so often”; TFTP/DNS timeouts are in the 1–5 s range. There is no technical requirement for 50 ms specifically.

**Who uses this loop:** The same `CUDPTask::Run()` event loop (and thus `HEARTBEAT_PERIOD`) is shared by **TFTP** (CTFTPRXTask / CTFTPTXTask), **NTP** (CNTPTask), and **Test WiFi** (CTestWifiTask). Changing the period affects all three.

**Fix applied:** In `pico/udptask.h`, **`HEARTBEAT_PERIOD`** was changed from **50** to **10** ms. The event loop now wakes up five times more often, so we service incoming UDP (and timers/abort) with much lower latency. NTP and Test WiFi only do a small number of UDP round-trips, so the main benefit is for TFTP: faster ACKs, fewer server timeouts and retries, and better effective transfer speed.

**Summary:** The slow TFTP and high error rate were not a fundamental limit of the Pico’s network capability but a consequence of (1) a 50 ms event-loop period adding latency to every packet, and (2) long blocking flash erases stalling the loop. Reducing the loop period to 10 ms improves responsiveness for TFTP (and slightly for NTP/Test WiFi) without a documented downside. Further gains would require not blocking the event loop on flash erase (e.g. defer erase or pre-erase in the background) so that ACKs can be sent immediately even when writing to flash.

**References:** `pico/udptask.h` (HEARTBEAT_PERIOD), `pico/udptask.cpp` (event loop, `cyw43_arch_wait_for_work_until`), `pico/tftprxtask.cpp` (WriteBlockForImageTransfer), `pico/mediaaccess.c` (WriteBlockForImageTransfer, sector erase every 16 blocks), `pico/flash.c` (tsEraseSector64k, ~220–250 ms).

---

## 13b. Why USB XMODEM/YMODEM completes with no delays vs TFTP

**Observation:** Disk image transfer over USB (XMODEM/YMODEM) completes consistently with no delays; TFTP over WiFi was slow and had a higher error rate (see §13).

**Same storage path:** Both paths call the same `WriteBlockForImageTransfer()` and thus the same blocking flash erase (~220–250 ms every 16 blocks). So the difference is not the flash layer but how the **transfer protocol and I/O** are handled.

**Differences:**

1. **ACK-before-write and flow control**  
   Both paths send ACK (or equivalent) **before** writing to flash, so the sender is told “send next” quickly. In **USB XMODEM** (`filetransfer.c`), `PacketReceived()` sends `usb_putchar(ACK)` immediately after validating the packet, then does the copy and `WriteBlockForImageTransfer()`. So the host can send the next packet while we are still in the flash write. In **TFTP**, we also send the ACK before `WriteBlockForImageTransfer()` in `tftprxtask.cpp`. So ordering is similar; the main difference is what happens *after* we send ACK.

2. **Synchronous loop vs event loop**  
   **USB XMODEM** runs in a **synchronous, blocking** loop on Core 0 (`xmodemrx()` invoked from `UserTerminal()` when not connected to Apple II): read one packet (block on `usb_getraw_timeout()`), validate, ACK, write to flash (block), then loop back and read the next packet. There is no separate “heartbeat” period; the only delay between “ACK sent” and “ready for next packet” is the flash write. The **next** packet can already be in flight or buffered while we are in `WriteBlockForImageTransfer()`.

3. **Buffering and polling**  
   **USB:** The host and the USB controller (TinyUSB, stdio_usb) **buffer** incoming data. While we block in `WriteBlockForImageTransfer()` for 220–250 ms, the host can send the next XMODEM packet(s); they sit in the USB FIFO. When we return and call `usb_getraw_timeout()`, we read from that buffer. So we never “miss” a packet due to being busy; we just drain the buffer as fast as we can.  
   **TFTP:** The UDP event loop only runs when we are **not** inside `WriteBlockForImageTransfer()`. The CYW43 WiFi driver requires **polling** (`cyw43_arch_poll()`) to move data from the WiFi chip into lwIP. During the 220–250 ms flash erase we do **not** call `cyw43_arch_poll()`, so we are not pulling the next DATA packet from the radio into the stack. That can lead to dropped packets, chip buffer limits, or the server timing out and retransmitting. So TFTP is sensitive to long blocking in the same thread as the event loop; USB is not, because USB hardware/driver buffers independently.

4. **Event-loop period (TFTP only)**  
   Even when not in a flash write, the TFTP path only processes the next UDP packet on the **next** iteration of the event loop, i.e. up to `HEARTBEAT_PERIOD` (10 ms) later. That adds latency per block. USB has no such period; we block on “read next packet” and process as soon as data is available.

**Summary:** USB XMODEM works well because (1) we ACK immediately so the host keeps sending, (2) we run in a tight synchronous loop so there is no heartbeat delay, and (3) USB buffers incoming data while we are in the long flash write, so we don’t drop data. TFTP uses the same “ACK before write” idea but (1) runs in an event loop with a 10 ms period and (2) cannot poll the WiFi driver during the blocking flash write, so the next DATA packet is not processed (and may be lost or cause timeouts) until we return. Improving TFTP further would require not blocking the event loop on flash erase (e.g. background or deferred erase) so that `cyw43_arch_poll()` and UDP handling continue during erases.

**References:** `pico/filetransfer.c` (`PacketReceived` ACK then `WriteBlockForImageTransfer`, `xmodemrx` loop), `pico/usbserial.c`, `pico/tftprxtask.cpp` (ACK then write), `pico/udptask.cpp` (event loop, `cyw43_arch_poll`), `pico/main.c` (UserTerminal when not Apple-connected).

---

## 13c. Buffering incoming TFTP traffic during blocking flash write

**Goal:** Allow incoming TFTP DATA packets to be received and buffered while the RX task is blocked in `WriteBlockForImageTransfer()` (especially during the ~220–250 ms sector erase), so the server does not timeout and the transfer does not stall.

**Approach:**

1. **Yield during flash wait**  
   In `flash.c`, `WaitUntilBusyClear()` (used by sector erase) calls an optional yield callback every ~500 iterations (~1 ms). The TFTP RX task registers `tftp_network_yield` (in `udptask.cpp`), which calls `cyw43_arch_poll()`, so the WiFi/lwIP stack can receive UDP packets while we are waiting for the flash chip.

2. **Enqueue DATA packets when inside blocking write**  
   When the RX task is about to call `WriteBlockForImageTransfer()`, it sets `enqueue_data_packets = true`. The UDP callback (`udp_callback` in `udptask.cpp`) first calls the task’s `OnUDPPacketReceived()`. The TFTP RX override: if `enqueue_data_packets` is true and the packet is a DATA packet from the same server, it copies the payload into a circular queue (16 entries × 1028 bytes) and returns true so the callback does not set `udpCallbackInvoked`. So packets that arrive during the blocking write are buffered instead of being processed immediately (and the callback does not overwrite `rxbuffer`).

3. **Drain queue in the event loop**  
   After handling `udpCallbackInvoked`, the event loop calls `DrainOneQueuedPacket()`. The TFTP RX implementation: if the queue is non-empty, pop one entry and call `EvtUDPReceived()` with that payload, then return true. So queued packets are processed in order on subsequent loop iterations without blocking the loop for the full erase duration.

4. **Scope of enqueue flag**  
   `enqueue_data_packets` is set true only around the actual `WriteBlockForImageTransfer()` calls in `ProcessDataPacket()` (both the normal block path and the EOF-with-512-byte path) and cleared when the block returns, so only DATA packets that arrive during the flash write are enqueued.

**Queue full:** If the queue is full (16 packets), the new packet is dropped (task still returns true so the callback does not set `udpCallbackInvoked`). The server will retransmit when it does not receive an ACK, preserving order.

**References:** `pico/flash.c` (`flash_set_yield_cb`, `WaitUntilBusyClear`), `pico/flash.h`, `pico/udptask.cpp` (`tftp_network_yield`, `OnUDPPacketReceived`, `DrainOneQueuedPacket`, `udp_callback`), `pico/udptask.h`, `pico/tftprxtask.cpp` (queue, `OnUDPPacketReceived`, `DrainOneQueuedPacket`, `enqueue_data_packets`), `pico/tftprxtask.h` (`TFTP_RX_QUEUE_SIZE`, queue layout).

---

## 14. Network architecture: single pump, multiple sessions (NTP, TFTP, Uthernet II)

**Requirement:** Support multiple concurrent network users on the Pico (NTP, TFTP, Test WiFi, and Uthernet II TCP/UDP sockets) without them blocking each other, while still respecting cyw43/lwIP’s requirement that there be a single “driver context” that calls `cyw43_arch_poll`, `cyw43_arch_wait_for_work_until`, and lwIP APIs.

### 14.1 Prior design: single blocking UDP task

- `CUDPTask` in `pico/udptask.cpp` owns:
  - WiFi connect (`InitCyw43`, `ConnectWifi`).
  - A single `udp_pcb` and its callback (`udp_recv(pcb, udp_callback, this)`).
  - A blocking event loop inside `Run()` that:
    - Calls `cyw43_arch_poll()` and `cyw43_arch_wait_for_work_until(...)`.
    - Delivers events (DNS result, UDP receive, timer, watchdog) to virtual methods (`EvtDNSResult`, `EvtUDPReceived`, `EvtTimeout`, etc.) on **one** task object.
- Global static state:
  - `volatile bool CUDPTask::isRunning;`
  - `CUDPTask *CUDPTask::runningObject;`
- DNS and UDP callbacks (`dns_callback`, `udp_callback`) check:
  - `if (CUDPTask::GetRunningObject() == pTask && pTask != NULL) {...}` and ignore callbacks if `arg` does not match `runningObject`.
- Higher-level users:
  - `CNTPTask` (NTP client), `CTFTPTask` (base for RX/TX TFTP), and `CTestWifiTask` all subclass `CUDPTask`.
  - `GetNetworkTime()` and `ExecuteTFTP()` both construct a task and call `task.Run(ssid, wpakey)`, which does not return until the task calls `Complete()` or throws.

**Consequence:** Only **one** UDP-based network activity can run at a time (e.g. NTP *or* TFTP). A broken or stuck NTP session monopolizes the event loop (`CUDPTask::isRunning` never cleared) and prevents TFTP or TestWifi from starting, even though the cyw43 chip and lwIP stack can support multiple sockets in parallel.

### 14.2 Target design: one network pump, many sessions

**Idea:** Separate the “driver context” (WiFi + lwIP pump) from the individual protocols. Instead of each protocol owning its own `CUDPTask::Run()` loop, introduce a **NetworkPump** that:

- Is the only context that:
  - Calls `cyw43_arch_init_with_country` / `cyw43_arch_enable_sta_mode`.
  - Calls `cyw43_arch_poll()` and `cyw43_arch_wait_for_work_until(...)`.
  - Creates and destroys lwIP pcbs (`udp_new`, `udp_bind`, `udp_recv`, `tcp_new`, `tcp_connect`, `tcp_listen`, `tcp_accept`, `tcp_close`, etc.).
- Maintains a registry of **sessions**:
  - NTP session (time sync).
  - TFTP RX/TX sessions (file transfer).
  - Test WiFi session (diagnostic).
  - Uthernet II sessions:
    - UDP sockets used by the Uthernet emulation.
    - TCP connections requested by the Apple II (client and server).
- Drives a cooperative loop (typically on Core 0 inside `core0Loop()`):
  - Polls cyw43/lwIP.
  - Delivers events to each active session.
  - Checks per-session timers and watchdogs.
  - Removes sessions that report “done”.

### 14.3 Session interface (conceptual)

Each protocol implementation becomes an `INetworkSession` (C++ interface) managed by the pump, instead of a subclass of `CUDPTask` that owns the loop. Roughly:

- `OnStart(NetworkPump&)`: called once when the session is added; can request DNS lookup, create pcbs, send initial packets.
- `OnDNSResult(err, ipaddr)`: DNS result for a hostname the session requested.
- `OnUDPReceived(payload, len, remote_addr, remote_port)`: called when a UDP packet arrives on a port (or TID) owned by this session.
- `OnTCPEvent(...)`: called from TCP accept/recv/sent/error callbacks for pcbs owned by this session.
- `OnTimer(arg)`: session-specific timer expiry (retries, timeouts).
- `OnWatchdog()`: called if the session has not made progress by some deadline; can abort or reset itself.
- `Abort()`: called when the system wants to tear the session down (e.g. Apple II reset, user cancel).
- `IsDone() const`: reports whether the session is finished (success or failure).
- Ownership helpers:
  - `OwnsUdpPort(uint16_t port) const` or an explicit binding table from `udp_pcb*` → session.
  - `OwnsTcpPcb(struct tcp_pcb* pcb) const` or similar.

The pump exposes helpers to sessions:

- `CreateUdpPcb(owner, local_port)` / `DestroyUdpPcb(pcb)`.
- `CreateTcpPcb(owner)`, `ListenTcp(owner, port)`, `ConnectTcp(owner, remote_ip, port)`.
- `ScheduleTimer(owner, timeout_ms, arg)` / `CancelTimer(owner)`.
- `RequestDNS(owner, hostname, timeout_ms)`.

Sessions never call `cyw43_arch_lwip_begin/end` directly; they ask the pump to perform network operations on their behalf so all lwIP and cyw43 calls flow through one place.

### 14.4 Event flow with the pump

With this design:

- **UDP callbacks:** `udp_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, ...)` is registered with `arg = NetworkPump*` instead of `arg = CUDPTask*`. The pump:
  - Looks up which session owns `pcb` (via a binding table).
  - Copies the payload into a buffer or small event object.
  - Frees the pbuf.
  - Marks that this session has a pending UDP event.
- **DNS callbacks:** `dns_callback(..., void *arg)` similarly uses `arg = NetworkPump*`; the pump matches the callback to the session that requested that hostname.
- **TCP callbacks:** `tcp_accept`, `tcp_recv`, `tcp_sent`, `tcp_poll`, `tcp_err` are all registered by the pump and associated with a given session via their `tcp_pcb*`.
- **Pump loop:** In each `PollOnce()`:
  - Call `cyw43_arch_poll()` to move data between the WiFi chip and lwIP.
  - Drain any pending UDP/TCP/DNS events and dispatch them to sessions’ handlers.
  - Check timers and watchdogs, and call `OnTimer` / `OnWatchdog` as needed.
  - Remove sessions whose `IsDone()` returns true and close their pcbs.

This keeps cyw43 and lwIP single-threaded (one owner) but lets multiple logical sessions (NTP, TFTP, Uthernet II sockets, etc.) share the network concurrently.

### 14.5 NTP and TFTP as sessions instead of tasks

- **NTP:** `CNTPTask` logic becomes an `NtpSession`:
  - On start: request DNS (`pool.ntp.org` round-robin) via pump.
  - On DNS result: send an NTP request via a UDP pcb bound to an ephemeral port.
  - On UDP receive: validate the NTP response, compute seconds since 1970, and mark done; or, on invalid packet, schedule another attempt.
  - On timer: handle NTP timeout and retry logic (up to `NTP_MAX_RETRY`) or signal failure.
- **TFTP:** `CTFTPTask` / `CTFTPRXTask` / `CTFTPTXTask` become `TftpSession` objects:
  - On start: DNS lookup of TFTP server; then send RRQ/WRQ.
  - On UDP receive: process DATA/ACK/OACK/ERROR packets, drive file I/O and retries.
  - On timer: handle TFTP timeouts and exponential backoff.
  - On abort: stop retrying, close the TFTP socket, and set `tftp_state.status`/`error` appropriately.

High-level C entry points (`GetNetworkTime`, `ExecuteTFTP`) no longer block inside `CUDPTask::Run`; instead they:

- Ask the pump to create and start a session.
- Either:
  - Wait/poll on a completion flag (for synchronous behaviour), or
  - Rely on the existing `tftp_state`/RTC mechanisms to report completion back to the Apple II.

### 14.6 Uthernet II TCP/UDP sessions

For Uthernet II emulation, the Pico must service:

- **UDP sockets**: used by U2 firmware (e.g. DNS, DHCP, SNTP, TFTP).
- **TCP connections**: used by Apple software via the Uthernet II API (e.g. telnet, HTTP).

In the pump-based design:

- Each Uthernet II logical connection becomes its own session (or a child object of a U2 session manager), with:
  - A `tcp_pcb` (client or accepted server connection) or `udp_pcb`.
  - State needed to translate between Apple II SmartPort / Uthernet II register access and lwIP calls.
- The pump routes TCP/UDP events for those pcbs to the corresponding session, which then:
  - Updates W5100-like registers.
  - Pushes data back to the Apple over the bus at the right time.
- NTP/TFTP/TestWifi sessions simply co-exist in the same pump, sharing the WiFi link and event-loop fairly.

### 14.7 Resetting all network sessions on Apple II reset

**Requirement:** When the Apple II reset line is asserted (GPIO `nRESET_PIN`), all in-flight network activity (TFTP transfers, Uthernet II TCP/UDP sockets, NTP queries) should be terminated cleanly so:

- No further packets are sent on behalf of the old Apple session.
- Any state that the Apple might poll (e.g. `tftp_state`) is driven to a completed/aborted state.
- The network pump returns to a known, idle state and is ready for new sessions after reset.

In the current design, `gpio_intr_callback` can call `UDPTask_RequestAbortIfRunning()` (which in turn calls `CUDPTask::RequestAbortIfRunning()`) to cancel a single UDP task. In the pump design, this becomes:

- A C-visible function in `network.cpp`, e.g. `Network_ResetAllSessionsOnAppleReset()`, that calls `NetworkPump::RequestAbortAll()`.
- `NetworkPump::RequestAbortAll()`:
  - Iterates over all active sessions and calls `Abort()` on each.
  - Optionally closes all pcbs immediately (or marks them for closure at the next `PollOnce()`).
  - Updates any global state mirrors such as `tftp_state` so that the Apple sees “completed with error/aborted” rather than “in progress forever”.
- `gpio_intr_callback` then does:
  - `Network_ResetAllSessionsOnAppleReset();`
  - `AbortEraseFlashDisk();` (as it already does).

This guarantees that a stuck NTP/TFTP/Uthernet II session cannot survive across an Apple reset and block new network operations from starting.

### 14.8 Migration path

Because this is a non-trivial refactor, a staged approach is safest:

1. **Stabilize the existing single-task design**  
   - Ensure all NTP/TFTP/TestWifi code paths either call `Complete()` or throw a `CUDPTask`-style exception so `CUDPTask::isRunning` is always cleared.
   - Strengthen logging and, if necessary, watchdog behaviour around retries so stuck NTP cannot silently monopolize the UDP loop.
2. **Introduce a minimal pump and session interface in parallel**  
   - Extract a `NetworkPump` that, initially, wraps the existing `CUDPTask::Run` loop and exposes a simple `PollOnce()`.
   - Convert `CNTPTask` into an `NtpSession` first, running under the pump while leaving TFTP on the legacy `CUDPTask` path.
   - Once NTP works under the pump, port TFTP and TestWifi to sessions.
3. **Add Uthernet II sessions**  
   - Implement UDP/TCP sessions for U2, using the same pump.
   - Remove the now-redundant `CUDPTask` singleton fields and the “ignore callback if arg != runningObject” logic.

**Stage 2 status (skeleton only):** Created `network_pump.h` / `network_pump.cpp` with a preliminary `NetworkPump` and `INetworkSession` interface. The pump currently:

- Owns lazy initialisation of cyw43 (`Init` / `EnsureWifiConnected`).
- Provides helpers to create/destroy UDP/TCP pcbs (`CreateUdpPcb`, `DestroyUdpPcb`, `CreateTcpPcb`, `DestroyTcpPcb`) using `cyw43_arch_lwip_begin/end`.
- Exposes `AddSession`, `RemoveSession`, `PollOnce`, `ScheduleTimer`, `CancelTimer`, and `RequestAbortAll` as **no-op placeholders** ready to be wired up when NTP/TFTP/Uthernet II are migrated.
- Is compiled into the Pico firmware (`network_pump.cpp` added to `CMakeLists.txt`) but is not referenced from existing code yet, so behaviour is unchanged.

**Takeaway:** The hardware (Pico W + cyw43 + lwIP) can support multiple concurrent UDP/TCP sockets. The primary limitation in the original design was the **software architecture**, which serialized all UDP work through a single `CUDPTask` instance and event loop. Moving to a pump + sessions model unlocks true concurrency between NTP, TFTP, and Uthernet II, and provides a natural place to reset all network state when the Apple II is reset. The new `NetworkPump`/`INetworkSession` skeleton is the first concrete code step toward that architecture; the next step will be migrating a single protocol (likely NTP) to run as a session under the pump.

*This document reflects reasoning and changes made during development; it may be extended as further design decisions are documented.*

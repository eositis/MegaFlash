# Implementation Notes and Reasoning

This document records the thinking, root-cause analysis, design decisions, and dead ends from work on the MegaFlash Pico firmware (Uthernet II, build, GPIO, C0C4 visibility). It is intended for future maintainers and for debugging similar issues.

---

## Chronology and context (from session work)

- **Version series:** **V1.1.24-eo** is the last **1.1.x** maintenance release. Ongoing work targets **1.2.x** starting from **V1.2.0-eo** (`0x0020` in `pico/defines.h`), focused on **Uthernet II emulation**, **com port**, and **imagewriter emulation**.
- **ip65 / Uthernet II:** U2 emulation was adapted so the ip65 stack (no changes to ip65) works: RECV command advances RX_RD to sn_rx_wr; socket CR is cleared to 0 after each command; default RMSR/TMSR = 0x06; MACRAW RX is fed by wrapping netif->input when socket 0 is opened in MACRAW. U2 debug logging uses prefix `[u2]` and is gated by UTHERNET2_DEBUG (Debug build only), not NDEBUG. See `docs/ip65-Uthernet-II-integration.md`.
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

## 1e. U2 activity monitor (`[u2m]`, Debug build)

**What:** Structured tracing of Uthernet II emulation for serial capture (UART 115200).

**Why:** Separate from `[u2]` `U2_DEBUGF` printf-in-place: bus cycles run on **core 1** while lwIP runs on **core 0**, so `printf` from both cores is unsafe. Events are **queued** under a critical section and **drained** from `U2_Poll()` (core 1) as `[u2m]` lines.

**What we did:** `u2_monitor.c` / `u2_monitor.h`; ring 128, up to 48 events flushed per `U2_Poll`; records **every** `$C0C4–$C0C7` bus op (Rd/Wr MODE/ADDRHI/ADDRLO/DATA, `busdata`, byte, internal `ptr`, MR), **reset**, **socket OPEN/CONNECT/LISTEN/CLOSE/SEND/RECV**, **UDP/TCP/MACRAW** net TX/RX. **CMake Debug** defines `U2_ACTIVITY_MONITOR=1`; Release compiles stubs. **`./build-debug.sh`** builds `pico_debug` + `pico2_debug`.

**References:** `pico/u2_monitor.c`, `pico/uthernet2.c`, `pico/uthernet2_net.cpp`, `pico/CMakeLists.txt`, `pico/build-debug.sh`, `pico/README.md`.

---

## 1d. RP2350: U2 branch skipped PIO IRQ 0 wait (ip65 “Device not found”)

**Symptom:** ip65 / telnet65 still reported **Device not found** despite correct RTR emulation and slot 4.

**Why:** On **RP2350**, `a2bus` sets **IRQ 0** while delaying the `rxfifo` pull (`a2bus_rp2350.pio`). The bottom of `BusLoop()` waits for IRQ 0 to clear before `UpdateMegaFlashRegisters`, so the SM finishes reading the **current** cycle’s FIFO value before the CPU overwrites it. The **Uthernet II** branch called `UpdateMegaFlashRegisters(1, …)` and then **`continue`**, **skipping** that wait. That can corrupt **$C0C4–$C0C7** read-back and make the ip65 **W5100 RTR XOR** probe at `$0017/$0018` fail (same user-visible message as a missing card).

**What we did:** Before `UpdateMegaFlashRegisters(1, …)` inside the U2 branch, added the same `pio_sm_is_rx_fifo_empty` + `pio_interrupt_get` wait as the main loop (guarded with `#ifndef PICO_RP2040`).

**References:** `pico/busloop.c`, `pico/a2bus_rp2350.pio` (`irq set 0` / `mov osr,rxfifo[y]` / `irq clear 0`).

---

## 1c. telnet65 “Device not found” vs ip65 W5100 `init` (MegaFlash U2)

**What:** `ip65/apps/telnet65.s` calls `ip65_init` with `A = eth_init_default` (`ip65/drivers/a2init.s`, slot **4** for MegaFlash). On failure it prints **“Device not found”** (`ip65_init` returns carry).

**Why:** `ip65_init` → `eth_init` → Contiki/ip65 **`drivers/w5100.s`** `init`. The **only** path that returns **`SEC`** before socket setup is the **RTR fingerprint** at addresses **$0017 / $0018**: two reads with XOR must produce zero (default **$07,$D0**). Any wrong values (open bus, wrong slot, or bad emulation) → probe fails → “Device not found.” After that, if **RMSR == $06**, ip65 **skips** the software-reset block that **writes SHAR** but still **reads SHAR back** into the driver MAC and then `ethernetcombo.s` copies that into **`cfg_mac`**. With SHAR all zeros, **station MAC is invalid** (DHCP/ARP issues) even though the probe passed.

**What we did:** `u2_reset()` already matched **RTR/RMSR/TMSR** for the probe; added default **SHAR** = same bytes as **`w5100.s`** (`00:08:DC:A2:A2:A2`) so the short init path gets a sane MAC.

**What we didn’t do:** Cannot fix “no device” from firmware if the CPU never addresses **slot 4** `$C0C4–$C0C7` or **nDEVSEL** does not assert (see §2b/§12).

**References:** `ip65/ip65/ip65.s` (`ip65_init`), `ip65/drivers/ethernetcombo.s` (`eth_init`/`init_adaptor`), `ip65/drivers/w5100.s` (`init` through RTR XOR and RMSR branch), `ip65/apps/telnet65.s`, `pico/uthernet2.c` (`u2_reset`).

**UART symptom:** Only one `[u2] mode=0x03…` line then “Device not found” is **normal** for `U2_DEBUGF` volume — the RTR probe uses **ADDRHI/ADDRLO + DATA** reads, which did not log. **Debug** builds also print **`RTR data read addr=0x0017 -> …`** / **`0x0018`** (first 8 such reads per boot) so captures show whether the 6502 reached the probe and got **0x07** / **0xD0**. If those lines never appear, bus cycles are not completing to the DATA port; if values are wrong, XOR fails and `eth_init` returns carry.

**Reconciling UART with the screen:** In stock **`ip65/drivers/w5100.s`**, the **only** **`sec` / `rts`** in **`init`** is the **RTR XOR** failure; after OPEN, **`init` ends with `clc` / `rts`**. **`ip65_init`** only branches to the device-failure path on **`eth_init` carry**; if **`eth_init` clears carry**, **`ip65_init` always ends with `clc` / `rts`** (it unconditionally **`clc`** after `timer_init` / `arp_init` / `ip_init`). So a capture that shows **correct RTR** (`0x0017`/`0x0018`) for the **same** telnet65 attempt **contradicts** a stock **`“Device not found”`** from **`ip65_init`** unless the Apple disk is **not** stock ip65, there is **carry corruption** (extraordinary), or the on-screen line is being **misattributed** to the same UART window.

**UART deep trace (Debug):** On **`MR=0x03`**, firmware arms **48** DATA-read trace events. **`printf` must not run inside `U2_HandleBusAccess`** (same for socket **CR** handling): blocking UART stalls the bus loop so the Apple can see wrong or incomplete cycles; only **`[u2m]`**-style **queue + `U2_MonPollFlush`** from **`U2_Poll`** is safe. **`[u2]`** mode and DATA lines use **`U2_MonQueueModeLine` / `U2_MonDataReadTrace`**; **`busloop.c`** polls **`U2_Poll`** every **32** Uthernet accesses (was 500) so the ring drains. Monitor ring/flush sizes were increased (**256** / **128**).

**Reconciling example log:** `debug/2026-03-21 21-42-20 FT232R USB UART #1.log` — **21:45** / **22:06** mode-only vs **22:14** / **22:26** with **correct RTR**; **DHCP** failures use **`- Error $…`** (`print_error`), not the **device not found** string.

---

## 1f. Manual monitor: two DATA reads both $07 (no advance to RTR1)

**Symptom:** After loading indirect address **$0017** via **$C0C5** / **$C0C6**, the **first** read of **$C0C7** returns **$07** (RTR0 — correct), but the **second** consecutive **$C0C7** read is **also** **$07** instead of **$D0** (RTR1).

**Why (W5100 + this firmware):** Auto-increment of the internal pointer after a DATA read happens only when **MR bit 1** (**`W5100_MR_AI`**, value **$02**) is set. **`read_value()`** in `uthernet2.c` always calls **`auto_increment()`** after the read, but **`auto_increment()`** is a no-op unless **`u2_mode_register & W5100_MR_AI`**. After **`u2_reset()`**, **MR is $00**, so **no** increment until the host writes **$03** (IND+AI, as in ip65) to **$C0C4**. If MR is **$01** (indirect without AI), behaviour matches the real part: repeated DATA reads keep returning the same byte.

**What to verify:** **`LDA $C0C4`** (read Mode Register) must show **$03** (or at least **bit 1 set**) *before* or *between* address setup and the two DATA reads. If it reads **$00** / **$01**, repeat **`LDA #3` / `STA $C0C4`** (slot 4), then reload **$0017** and try again. Optionally read **$C0C5** / **$C0C6** after the first **$C0C7** read: with AI on, the pointer should show **$0018**.

**References:** `pico/uthernet2.c` (`read_value`, `auto_increment`, `u2_reset`), `pico/w5100_regs.h` (`W5100_MR_AI`, `W5100_RTR0` / `W5100_RTR1`).

**Observed variant (bench):** Three consecutive **DATA** reads returned **`$D0`, `$07`, `$D0`**. In our image, **`$0017`→RTR0=`$07`**, **`$0018`→RTR1=`$D0`**, so that triplet is **RTR1, RTR0, RTR1** — the internal pointer behaved like **`$0018` → `$0017` → `$0018`**, not like a monotonic **`$0017` → `$0018` → `$0019`** (which would be **`$07`, `$D0`, `$08`** with default **RCR=`$08`**). Causes to rule out on the bench: (1) **pointer was `$0018`** before the first read (e.g. **`C0C6:18`** or leftover state) — then the **second** byte should be **`$08`** (RCR at `$0019`), not **`$07`**, unless something **reloaded** the address to **`$0017`** between reads; (2) **monitor or test code** touched **`$C0C5`/`$C0C6`** between **`$C0C7`** reads; (3) **stale / reordering** on the data path (compare with **§1d** and pointer dumps below).

**Screenshot (correct setup):** After **`C0C4:03`**, **`C0C5:00`**, **`C0C6:17`**, verify reads **`C0C5`/`C0C6` → `00`/`17`**, then **`C0C7`** returned **`$08`**, **`$07`**, **`$D0`** — i.e. **RCR, RTR0, RTR1** (`$0019`, `$0017`, `$0018`) while the **address registers still showed `$0017`** before the first DATA read. **Why:** On **RP2350**, the **a2bus** SM **prefetches** the chunk‑1 FIFO value for the **next** 6502 read **before** the CPU finishes handling the **previous** cycle (see **§1d**). **`registers.r[7]`** (the **`$C0C7`** shadow) was only updated **during** a DATA read, so after **`C0C6`** it still held whatever was left from an earlier cycle — not **`read_value_at($0017)`=`$07`**. The 6502 then latched a **stale** eighth byte for the first **`$C0C7`** read; the emulator’s internal pointer still advanced correctly, so later reads matched **RTR0/RTR1**.

**What we did:** After every U2 bus op, **`registers.r[7] = U2_PeekDataPort()`** (next DATA byte without increment) before **`UpdateMegaFlashRegisters(1,…)`**, so the PIO’s prefetched **`$C0C7`** byte matches **`read_value()`** for the **next** access. **`U2_PeekDataPort`** in **`uthernet2.c`** / **`uthernet2.h`**; **`busloop.c`** U2 branch.

**What we didn’t do:** No change to W5100 memory layout or RTR defaults; RP2040 gets the same priming (harmless and keeps shadow consistent).

**Verified (bench):** Monitor sequence after the fix returns **`$07`**, **`$D0`**, **`$08`** for three consecutive **`$C0C7`** reads with pointer **`$0017`** and **MR=`$03`**.

---

## 1g. U2 monitor UART on core 1 → `async_context` PANIC; MACRAW RX “no room”

**Symptom:** After **`[u2] ck=5`** (MACRAW OPEN ok), UART showed **`*** PANIC ***`** / **`async_context_poll context check failed (IRQ or wrong core)`**. Separately, **`[u2] MACRAW RX … drop … (no room)`** repeated in a loop while telnet65 stayed on **“obtaining IP”** (DHCP).

**Why (PANIC):** **`U2_MonPollFlush()`** calls **`printf`** to drain the **`[u2]` / `[u2m]`** queue. It was invoked from **`U2_Poll()`** on **core 1** (Apple bus loop). On Pico W, **UART stdio** / **cyw43** **`async_context`** must not be polled from the wrong core — flushing the monitor from core 1 tripped the SDK check the same instant **`ck=5`** was printed.

**What we did:** **`U2_Poll()`** now runs **`U2_Net_Poll()`** only. **`U2_MonPollFlush()`** runs on **core 0** next to **`NetworkPump_PollOnce()`** in **`core0Loop`**, at the start of each NTP cycle, and in the **USB idle** loop when MegaFlash menu runs without **`core0Loop`**. **`u2_monitor.h`** documents core‑0‑only flush.

**Why (no room):** **`u2_netif_input_wrapper`** feeds **every** incoming Ethernet frame into socket 0 **MACRAW** while WiFi is up; the emulated **4K** RX ring fills faster than the II drains it during DHCP.

**What we did:** In **`u2_push_rx_macraw`**, if **`2+len`** does not fit, call **`u2_socket_discard_rx()`** (set **RX_RD** to **sn_rx_wr**, discarding unread data) and **retry** once so a new frame (e.g. DHCP) can land. Removed **`U2_DEBUGF`** spam on the hot path (monitor already queues **`U2_MonNetRxMacraw`** from **`uthernet2_net.cpp`**).

**References:** `pico/main.c`, `pico/uthernet2.c` (`u2_socket_discard_rx`, `u2_push_rx_macraw`), `pico/u2_monitor.h`.

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
- Making the build reproducible: pass the SDK path explicitly on the command line so the script does not depend on the environment. Defaults are portable: **`$HOME/pico-sdk`** in **`cmakeall.sh`** / **`build-both.sh`** and **`$ENV{HOME}/pico-sdk`** in **`CMakeLists.txt`** so Intel vs ARM macOS and different usernames do not require editing the repo. Override with **`PICO_SDK_PATH`** or **`-DPICO_SDK_PATH`** if the SDK lives elsewhere.

**Change:** Scripts pass **`-DPICO_SDK_PATH="$SDK_PATH"`** with **`SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"`**.

**Follow-up:** `pico/CMakeLists.txt` applies the same **`$HOME/pico-sdk`** fallback when neither `-DPICO_SDK_PATH` nor a non-empty `ENV{PICO_SDK_PATH}` is set, *before* `include(pico_sdk_import.cmake)`, so bare `cmake -B build -S .` works in non-interactive environments. **Toolchain discovery:** after **`ARM_TOOLCHAIN_PATH`** and **`/Applications/ArmGNUToolchain/...`**, scripts check **`/opt/homebrew/bin`** then **`/usr/local/bin`** so Apple Silicon Homebrew is preferred over a stale Intel-era **`PATH`**. **`mf_try_arm_toolchain_bin`** also requires **`nosys.specs`** (newlib) so Homebrew’s **`arm-none-eabi-gcc`** alone is rejected; Pico needs the full Arm GNU Embedded **.pkg** (darwin-aarch64 on Apple Silicon). **`pico/build-env.sh`** sets **`CMAKE_BIN`** the same way (**`/opt/homebrew/bin/cmake`** first) so **`cmakeall.sh`**, **`build-both.sh`**, and **`build-debug.sh`** do not invoke an x86_64 CMake on ARM Macs; override with **`CMAKE=/path/to/cmake`**. **`cpanel`:** scripts run **`make release`** only so the test-disk **`java`** step is skipped (avoids x86 Java after Intel→ARM migration). **`set -e`:** **`GCC_PATH=$(command -v …)`** uses **`|| true`** so a missing compiler does not abort before the error message.

**`build-both.sh`:** For verification without bumping `defines.h`, run `./build-both.sh` from `pico/` — it builds **cpanel**, then configures and **`make`s both `pico_release` and `pico2_release`** (same toolchain logic as `cmakeall.sh`). See summary table §11.

**References:** `pico/cmakeall.sh`, `pico/CMakeLists.txt`, `pico/pico_sdk_import.cmake`.

---

## 4b. Vendored picotool (host arch)

**What:** `pico/CMakeLists.txt` sets **`PICOTOOL_FETCH_FROM_GIT_PATH`** to **`pico/picotool`**. The shipped **`picotool/picotool/picotool`** binary may be **x86_64** (e.g. copied from an Intel Mac). On Apple Silicon, the firmware link step fails with **`Bad CPU type in executable`**.

**What we did:** Rebuild from **`pico/picotool/picotool-src`** with **`/opt/homebrew/bin/cmake`**, **`-DPICO_SDK_PATH`**, **`-DPICOTOOL_FLAT_INSTALL=1`**, **`-DCMAKE_INSTALL_PREFIX=.../pico/picotool`**, **`cmake --build`** + **`cmake --install`**.

**libusb (Apple Silicon):** **`picotool-src`** uses **`find_package(LIBUSB)`**. If the only **`libusb-1.0`** on the machine is under **`/usr/local`** (Intel Homebrew or old install), it is often **x86_64** → link fails with **undefined `libusb_*` / wrong architecture**. **Preferred:** install **`libusb`** with Apple Silicon Homebrew (**`brew install libusb`**) so **`/opt/homebrew/lib/libusb-1.0.dylib`** is **arm64**. Ensure **`pkg-config`** is available from the same prefix (**`brew install pkgconf`**) so CMake does not fall back to a broken **`/usr/local/bin/pkg-config`**. Rebuild **picotool** **without** **`PICOTOOL_NO_LIBUSB`** for full USB/picoboot support. **Fallback:** **`-DPICOTOOL_NO_LIBUSB=1`** still produces a **UF2**-capable **picotool** with no USB stack (fine for firmware builds only).

**Takeaway:** The **.pkg** Arm toolchain was fine; the blocker was the **host** picotool. A repo that supports both Intel and ARM Macs should not commit a single-arch picotool binary, or should document rebuilding **`picotool-src`** per host.

**References:** `pico/picotool/picotool-src/CMakeLists.txt`, `pico/CMakeLists.txt` (`PICOTOOL_FETCH_FROM_GIT_PATH`).

---

## 4c. cpanel `make all`, Java, and `java_home` (Apple Silicon)

**What:** **`cpanel/Makefile`** runs **`java -jar ./acx18.jar`** for the test-disk target. **`/usr/bin/java`** delegates to **`/usr/libexec/java_home`**, which only lists JDKs under **`/Library/Java/JavaVirtualMachines/`** (plus some legacy bundles). A **Homebrew arm64 OpenJDK** under **`/opt/homebrew`** is **not** listed until registered. An old **Intel Homebrew** OpenJDK under **`/usr/local/Cellar/openjdk`** *was* picked and failed with **`Bad CPU type`**.

**Uninstall Intel OpenJDK without Intel brew:** On Apple Silicon, **`/usr/local/bin/brew uninstall`** may fail (x86 portable Ruby). If **`/usr/local/Cellar/openjdk`** is owned by your user, remove it directly: **`rm -rf /usr/local/Cellar/openjdk`** and **`rm -f /usr/local/opt/openjdk`** (broken symlink after Cellar removal).

**Register arm64 OpenJDK:** Run **`./tools/register-arm-openjdk-macos.sh`** from the repo root (one **`sudo`** to **`ln -sfn "$(brew --prefix openjdk)/libexec/openjdk.jdk"`** → **`/Library/Java/JavaVirtualMachines/openjdk.jdk`**). Then **`/usr/libexec/java_home -V`** should include **arm64** OpenJDK. **Alternatively** set **`JAVA_HOME`** to **`$(brew --prefix openjdk)/libexec/openjdk.jdk/Contents/Home`** and **`PATH="$JAVA_HOME/bin:$PATH"`** without registering.

**Leftover x86 JVM:** **Oracle Java 8** browser plugin under **`/Library/Internet Plug-Ins/JavaAppletPlugin.plugin`** may still appear in **`java_home`**; remove that install from Oracle’s uninstaller if you want **`-a arm64`** to be unambiguous.

**References:** `cpanel/Makefile`, `tools/register-arm-openjdk-macos.sh`.

---

## 4d. Upstream [ThomasFok/MegaFlash](https://github.com/ThomasFok/MegaFlash): storage & IIc+ / Applesoft

**What:** Periodic diff of **`main`** vs **`upstream/main`** (remote **`ThomasFok/MegaFlash`**) to spot fixes/features not yet merged.

**Where:** Full write-up (open beside the editor): **`ThomasFok-upstream-comparison.md`** (repo root). Highlights: **`fswrts` / `swjmp_ay`** (IIc+ bank switch / boot path), **RAM disk dedicated DMA**, **flash DMA + overclock hang fix**, **TFTP DOS-order image + imagewriter**, **`romdisk` DMA removal** upstream. **`firmware/accel.s`** is unchanged vs upstream in a recent diff.

**References:** `ThomasFok-upstream-comparison.md`, stub `docs/Upstream-ThomasFok-storage-accel-comparison.md`.

---

## 4e. ThomasFok storage stack: first merge (RAM/ROM disk + flash read DMA)

**What:** Bring over Thomas’s storage-related Pico changes without dropping fork-only behaviour (SmartPort unit order with ROM disk first/last, Slinky register init, **`ts*`** flash/RAMdisk symbol names).

**Why:** Shared **`dmamemops`** DMA with RAM disk risks contention when both cores use memory DMA; Thomas gives RAM disk its own channel. Flash read previously used paired TX/RX DMA; at high CPU clock that path can hang—Thomas uses CPU TX + RX DMA with a short timeout and **`spi_read_blocking`** + **`CRC32Aligned`** fallback.

**What we did:** **`pico/ramdisk.c`**: dedicated DMA (copy/zero), mutex on exported paths only, **`InitRamdisk()`**; **`pico/ramdisk.h`** / **`pico/main.c`**: declare and call init after **`InitDMAChannel()`**. **`pico/romdisk.c`**: **`memcpy`** for block read; removed **`dmamemops`** include; left **`romdiskFirst`** and defaults as on this fork. **`pico/flash.c`**: replaced **`ReadFromFlashByDMA`** with Thomas’s timeout/abort implementation; **`tsReadOneBlock`** / **`tsReadSector`** pass **`success`** and fall back on failure.

**What we didn’t do (yet):** Remainder of **`flash.c`** (see §4f for **`dmamemops`**), **`slinky.c`** / **`mediaaccess.c`** upstream refactors (would conflict with **`GetRomdiskFirst()`** or Slinky **`UpdateMegaFlashRegisters`** init). TFTP DOS-order **`imagewriter`** remains separate.

**References:** `pico/ramdisk.c`, `pico/romdisk.c`, `pico/flash.c` (`ReadFromFlashByDMA`, `tsReadOneBlock`, `tsReadSector`), `pico/main.c`.

---

## 4f. Thomas `dmamemops.c` + further `flash.c` (without API rename)

**What:** Align shared memory DMA with Thomas’s **`dmamemops.c`** (separate frozen **`dma_channel_config_t`** per operation width: copy 8/32, zero 8/32, CRC 8/32) so routines no longer mutate and restore a single global config. Add **`OC_RP2350`** (**`CMakeLists.txt`**: **`pico2_w`**) and matching **`enable_spi0` / `disable_spi0`** extra **`nop`** delays. Fix partial **`tsWriteSecurityRegister`** merge path to call **`tsProgramSecurityRegister(..., 256)`** (full page), not **`len`**.

**Why:** Per-call config twiddle was fragile; Thomas’s approach matches hardware channel setup to each use. RP2350 overclock + 75 MHz SPI needed CS timing margin per upstream. Programming **`len`** bytes after a 256 B read/merge could truncate the security register image.

**What we didn’t do:** Thomas renames flash exports (**`ReadBlockFlash`**, **`EraseEverything`**, static **`GetBlockLoc`**, **`ReadUserConfigBlock`** in **`flash.c`**, drops **`blockBuffer`**) — merging that would force **`flash.h`**, **`userconfig.c`**, **`mediaaccess.c`**, **`terminal.c`**, **`misc.c`**, **`encryption.c`** renames and wider regression risk; deferred until a dedicated API pass.

**References:** `pico/dmamemops.c`, `pico/flash.c` (`enable_spi0`, `disable_spi0`, `tsWriteSecurityRegister`), `pico/CMakeLists.txt` (`OC_RP2350`).

---

## 4g. Thomas `firmware/smartport.s` (segment + ZP restore)

**What:** Port [ThomasFok/MegaFlash](https://github.com/ThomasFok/MegaFlash) **`smartport.s`** changes without touching **`fswrts` / `megaflash.s`**.

**Why:** **`HOMESEGMENT`** lets a future linker map the SmartPort “home” code to a segment other than **`ROM1`** in one place. The **`RESTOREZPSCRATCH`** epilogue uses a negative starting **`X`** and **`sta z:zpscratch+ZPSCRATCHSIZE,x`** so the loop ends with **`bne`** (no **`cpx #ZPSCRATCHSIZE`**). **`ZPSIZE`** restore uses **`bne`** instead of **`blt`** (same iteration count for the counted loop). Comment block about IIc+ IOROM/debug placement removed upstream—behaviour unchanged.

**What we didn’t do:** **`patches.s`**, **`megaflash.s`**, **`bootmenu.s`** Thomas package (IIc+ bank return)—still separate.

**References:** `firmware/smartport.s`, `git diff main upstream/main -- firmware/smartport.s`.

---

## 4h. Thomas `fswrts` (IIc / IIc+ ROM, ZIP chip)

**What:** Port [ThomasFok/MegaFlash](https://github.com/ThomasFok/MegaFlash) bank-switch + cold return path so aux-ROM init does not rely on **`jmp ($0000)`** from bank 1 or on the stock **`$C784`** SWRTS region (non-cacheable on IIc Plus / ZIP).

**Why:** After init, execution must continue in **bank 0** at the reset vector target. **`fswrts`** at **bank 1 `$FFC8`** executes **`sta rombank`**; the machine’s existing **`RTS` at `$FFCB`** (bank 0) then returns into the intended address (stack set up with **address−1**). Boot menu entry uses the same mechanism (**`BMRUN−1`**) instead of **`jmp BMRUN`** from aux.

**What we did:** **`patches.s`**: **`$FB19`** → **`lda #MODE_INIT` / `jmp slxeq`**; **`B1_FFC8`** **`fswrts`**; **`.export fswrts`**. **`megaflash.s`**: **`.import fswrts`**, **`.export swjmp_ay`**, **`swjmp_ay_sp0` / `swjmp_ay`**, cold-exit loads **`($0000)-1`** into **A/Y** then falls into **`swjmp_ay_sp0`**. **`macros.inc`**: **`ld16iay`**. Linker: **`B1_FFC8`** in **`iic.cfg`**, **`iicplus.cfg`**, **`merge_iic.cfg`**, **`merge_iicp.cfg`**, and matching **`merge_iic.s` / `merge_iicp.s`** **`incbin`**.

**What we didn’t do:** Thomas **`HOMESEGMENT`** throughout **`megaflash.s`** / large **`bootmenu.s`** refactors — only the **`fswrts`** package as above.

**References:** `firmware/patches.s`, `firmware/megaflash.s` (`coldstartinit`, `swjmp_ay`), `firmware/macros.inc`, `firmware/merge_iicp.s`, `firmware/iicplus.cfg`.

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

So “Pico debug mode” = Debug build → UART console, all DEBUG_PRINTF-style logging, bus loop always on, optional lwIP/TinyUSB debug. Release → no UART stdio, no debug prints. As of the 1.1.20 fix (see §7b), Release now also always starts the bus loop and uses CheckPicoW() for the Core0 branch so the network stack runs on Pico W regardless of appleConnected.

**References:** `docs/Debug-mode.md`, `pico/main.c`, `pico/debug.h`, `pico/lwipopts.h`.

---

## 7b. Release build network stack “nothing works” (1.1.20)

**Symptom:** Debug build: network works; Release build: network stack broken, nothing works.

**Root cause:** In Release we previously launched Core1 only when `IsAppleConnected()` was true and ran `core0Loop()` only when `appleConnected` was true. If `IsAppleConnected()` was false at boot (e.g. PHI0 timing), Core1 and the network loop never ran.

**Fix:** Always call `multicore_launch_core1(core1Main)`. Use **CheckPicoW()** for the Core0 branch: if `CheckPicoW()` run `core0Loop()`, else User Terminal. On Pico W the network stack then always runs. Apple reset interrupt remains gated on `appleConnected`.

**Why it was missed in the release build:**
- Development and testing used **Debug** builds (UART logs, bus loop always on). The network stack was validated in Debug only.
- Release had **intentional** branching (Core1 and core0Loop only when `appleConnected`) to “save resources when not connected,” so the difference looked like design, not a bug.
- There was **no requirement to test the Release build** for network (NTP, TFTP, WiFi test) before shipping. Release was built and packaged but not exercised on the same critical paths as Debug.
- The failure is **timing-dependent**: if the Apple is on and PHI0 is toggling when the Pico boots, `IsAppleConnected()` can be true and Release appears to work; if the Pico boots first or the check runs too early, it fails. So the bug could be intermittent.

**Prevention (avoid repeating this):**
1. **Always test Release before release.** Before tagging or packaging a version, run the **Release** firmware (not Debug) and verify: NTP sync, TFTP transfer, WiFi test from the Control Panel. Use the same hardware and boot order you expect in the field.
2. **Document the requirement.** Keep a short “Pre-release verification” checklist and follow it for every release (see §7c).
3. **Minimize critical Debug-vs-Release differences.** Avoid putting “does this feature run at all?” behind `#ifdef NDEBUG` or Release-only conditions. If behaviour must differ, document and test both code paths.
4. **CI:** If you add CI, build both Debug and Release and run any automated tests against both so Release is exercised, not only built.

**References:** `pico/main.c`.

---

## 7c. Pre-release verification checklist

Before packaging or tagging a firmware release (e.g. 1.1.20+), run the **Release** build (e.g. `pico_release/megaflash.uf2` or `pico2_release/megaflash.uf2`) on real hardware and confirm:

- [ ] **Boot:** Pico W boots with MegaFlash in an Apple IIc/IIc+ (or target machine).
- [ ] **Network:** From the Control Panel (or equivalent), NTP sync, TFTP upload/download, and WiFi test all complete successfully. If any fail, do not ship until fixed or documented.
- [ ] **Bus/Apple:** Commands from the Apple (e.g. SmartPort, boot menu) are handled correctly.

This avoids shipping a Release build that was only tested as Debug (see §7b).

---

## 7d. TFTP upload stall after ~14 blocks (lockup, hard power-off required)

**Symptom:** TFTP upload (TX) stalls after about 14 blocks; reboot does not recover; hard power-off required to restart MegaFlash.

**Root cause (likely):** The next TFTP data packet was built inside the UDP receive handler path: on ACK we called `SendDataPacket()` → `BuildDataPacket()` → `ReadBlock()` → flash (SPI + mutex). Doing that work from the same call stack that processed the UDP packet could interact badly with lwIP/CYW43 or cross-core ordering and lead to stall or deadlock after several iterations.

**Fix:** Defer building the *next* data packet to the start of the next event-loop iteration. In `udptask` add virtual `OnBeforeWait()` called before `cyw43_arch_poll()`. In TFTP TX, set `needToBuildNextPacket` (and params) in `SendDataPacket()` instead of calling `BuildDataPacket()`; `CTFTPTXTask::OnBeforeWait()` builds the packet so flash/SPI runs outside the UDP handler path. First data packet still built in handler when `blockSent==0`.

**References:** `pico/udptask.h`, `pico/udptask.cpp`, `pico/tftptxtask.h`, `pico/tftptxtask.cpp`.

---

## 7e. Debug vs Release: optimization, not just NDEBUG

**Observation:** Debug build works (e.g. TFTP upload); Release build stalls. “Removing debug” should only disable UART reporting; all other code should remain unchanged.

**What actually differs:** CMake’s default for **Release** is **-O3 -DNDEBUG**; **Debug** is **-Og -g** (no NDEBUG). So there are two differences:
1. **NDEBUG** – In Release, assert() is a no-op and `debug.h` macros (DEBUG_PRINTF, etc.) become no-ops. That’s “disable UART reporting.”
2. **Optimization** – Release uses **-O3** (aggressive inlining, reordering, etc.); Debug uses **-Og**. Different codegen can change timing, expose races, or alter behavior in subtle ways even when logic is the same.

**Fix:** In `CMakeLists.txt`, set Release to use the same optimization as Debug (**-Og**) and keep **-DNDEBUG** only:
- `CMAKE_C_FLAGS_RELEASE="-g -Og -DNDEBUG"`
- `CMAKE_CXX_FLAGS_RELEASE="-g -Og -DNDEBUG"`  
(CACHE STRING … FORCE so existing release build dirs pick it up.)

Then “Release” = no UART, no assert, but same code generation as Debug. Reconfigure release (e.g. re-run cmake for `pico_release`) and rebuild.

**References:** `pico/CMakeLists.txt`, CMake `CMAKE_<LANG>_FLAGS_RELEASE`.

---

## 7f. Disabling debug must not change lwIP (TFTP breaks in release)

**Symptom:** With debug enabled, TFTP works. As soon as debug is disabled (release build), TFTP data transfer breaks (e.g. stalls at 8 blocks, timer runs, no retransmit). “Disabling debug” should only turn off UART reporting; nothing else should change.

**Cause:** In `lwipopts.h`, LWIP_DEBUG, LWIP_STATS, and LWIP_STATS_DISPLAY were set only when `#ifndef NDEBUG`. So in release (NDEBUG defined), lwIP was built without those options. That changed lwIP’s code and/or data layout (stats, debug paths) and broke UDP/TFTP behavior.

**Fix:** Do not tie lwIP options to NDEBUG. Set them to fixed values so Debug and Release use the same lwIP configuration:
- `LWIP_DEBUG 0`, `LWIP_STATS 0`, `LWIP_STATS_DISPLAY 0` in all builds.

Then “disabling debug” (NDEBUG) only affects our code (assert, DEBUG_PRINTF etc.), not the network stack.

**References:** `pico/lwipopts.h`.

---

## 7g. TFTP hostname field shows wrong default (Pico IP first time, “transferred successfully” after job)

**Symptom:** On the Control Panel TFTP Disk Image Transfer page (Page 3), the “Enter IP Addr or hostname of server” field shows the wrong default: the **first time** the page is opened it shows the **Pico’s own IP**; **after a successful TFTP job**, returning to that page shows **“transferred successfully”** (or similar status text) instead of the last-used server hostname.

**Root cause:** The MegaFlash **data buffer** is shared. When the Apple sends `CMD_TFTPGETLASTSERVER`, the Pico runs `DoTFTPGetLastServer()` which overwrites `dataBuffer` with `GetTFTPLastServer()` and calls `ResetDataPointer()` (so `registers.r[DATAREG] = dataBuffer[0]`). The Apple then reads the data register to get the hostname via `CopyStringFromDataBuffer(ti_textBuffer)`. The **first byte** the Apple sees on a read of DATAREG is whatever the **PIO** is currently outputting. The PIO’s copy of the registers is updated only when we call `UpdateMegaFlashRegisters()`. After handling the command write we cleared BUSY and updated in-memory `registers`, but the **next** bus cycle that ran might be a **read of STATUSREG** (wait loop); in the read path we have no case for STATUSREG so we hit `default: continue` and **skip** the end-of-loop `UpdateMegaFlashRegisters`. So the PIO was not guaranteed to have the new DATAREG (and cleared BUSY) before the Apple’s first read of DATAREG. The Apple could therefore still see the **previous** content of the data buffer—e.g. the Pico’s IP from a prior WiFi test, or the status string “Completed Successfully” from the last `CMD_TFTPSTATUS` poll—instead of the hostname string just written by `DoTFTPGetLastServer`.

**Fix:**

1. **Pico (`busloop.c`):** Immediately after clearing the BUSY flag in the CMDREG write handler, call `UpdateMegaFlashRegisters(0, registers.i32[0])` so chunk 0 (including DATAREG and STATUSREG) is pushed to the PIO **before** any further bus cycles. Then the Apple’s first read of DATAREG after `SendCommand(CMD_TFTPGETLASTSERVER)` returns will see `dataBuffer[0]` (first character of the last server hostname), not stale data.

2. **Control Panel (`cpanel/tftp.c`):** After `CopyStringFromDataBuffer(ti_textBuffer)`, if the string looks like TFTP status text (e.g. contains “Successfully”, “Completed”, “transferred”, “Transferring”, “Connecting”), clear `ti_textBuffer` to empty so the field shows blank instead of wrong default. This is a defensive fallback if any stale data still appears.

**Takeaway:** Any command that fills the data buffer and resets the data pointer must be followed by an immediate PIO update of chunk 0 so the Apple’s first read of DATAREG sees the new first byte.

**References:** `pico/busloop.c` (CMDREG case, `UpdateMegaFlashRegisters` after clear BUSY), `pico/cmdhandler.c` (`DoTFTPGetLastServer`, `ResetDataPointer`), `pico/tftpstate.c` (`TFTPFormatStatusMessage`), `cpanel/tftp.c` (hostname prefill, `LooksLikeStatusText`).

## 7h. TFTP upload: block line shows only numerator (missing `N/total`)

**Symptom:** Control Panel TFTP upload page showed only blocks transferred (e.g. `42`) instead of `42/2800 (1.5%)` as before.

**Cause:** `TFTPFormatBlocksMessage()` in `tftpstate.c` needs `tftp_state.tsize` (total transfer size in bytes) to append `/totalBlocks` and percent. `CTFTPTXTask::Run()` used to set `tftp_state.tsize = blockCount * PRODOS_BLOCKSIZE` before `CTFTPTask::Run()`. After **`NetworkPump::RunTFTP`** stopped calling `CUDPTask::Run()` and instead uses **`BeginRun` + `StartEventsAfterBeginRun` + `PollOnce`**, **`CTFTPTXTask::Run()` was never invoked**, so `tsize` stayed `TFTPSTATE_INVALIDTSIZE` and the formatter exited after printing the first number.

**Fix:** Implement **`CTFTPTXTask::EvtStart()`** to set the same `tftp_state.status` / `tftp_state.tsize` and call **`CTFTPTask::EvtStart()`** (DNS). **`CTFTPTXTask::Run()`** now only forwards to **`CTFTPTask::Run()`**. For symmetry, **`CTFTPRXTask::EvtStart()`** sets **`TFTPSTATUS_WIFICONNECTING`** (previously only in **`Run()`**).

**References:** `pico/tftptxtask.cpp`, `pico/tftprxtask.cpp`, `pico/tftpstate.c` (`TFTPFormatBlocksMessage`), §14.8.

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

## 10c. Control panel: firmware version left of clock (`DisplayTime`)

**Requirement:** MegaFlash Control Panel should show the Pico firmware build string immediately to the **left** of the live clock on the bottom text line.

**What happened:** `_DisplayTime` in `cpanel/asm-megaflash.s` had been trimmed to only run `CMD_GETTIMESTR` and paint cols 32–39, so the version disappeared. The firmware still implements `CMD_GETFIRMWAREVER` (`pico/cmdhandler.c` → `DoGetFirmwareVer`, 12 bytes with high bit set).

**Fix:** Call `CMD_GETFIRMWAREVER` first, copy 12 bytes from `paramreg` to screen RAM `$7D0+20` (cols 20–31), then `CMD_GETTIMESTR` to `$7D0+32` (cols 32–39). Extend `_ClearTime` to blank cols 20–39 so the format flow that clears the clock does not leave stale version text.

**References:** `cpanel/asm-megaflash.s` (`_DisplayTime`, `_ClearTime`), `pico/cmdhandler.c` (`DoGetFirmwareVer`).

---

## 10d. Drives Enable: `gotoxy` Y is window-relative (cc65)

**Symptom:** Flash/RAM drive enable ticks updated on the wrong row when toggled (about **YPOS** lines too low with `YPOS=6`).

**Root cause:** In cc65’s `libsrc/apple2/gotoxy.s`, `_gotoxy` does `CV = WNDTOP + y` (after `popa` for Y). The second argument is **relative to the scroll window top**, not an absolute screen row. Code in `drivesenable.c` passed `YPOS + row` as if matching `wnd_DrawWindow`’s content origin—effectively adding `WNDTOP` twice for the vertical component.

**Fix:** Use window-relative rows only: drive `i` → `PrintCheckbox(i, …)`; RAM row → `listCount`; ROM row → `listCount + 1` (and `romdiskRow = listCount + 1` for the ROM line `gotoxy` calls). Same pattern as `gotoxy(1, HEIGHT-1)` elsewhere (relative index inside the window).

**References:** cc65 `apple2/gotoxy.s`, `cpanel/drivesenable.c`, `cpanel/ui-wnd.c` (`wnd_DrawWindow` sets `WNDTOP`).

---

## 10e. Git: `1.1.x` maintenance branch

**Purpose:** Keep a named line for **1.1.x** hotfixes (baseline **V1.1.24-eo**) while **`main` carries 1.2.x** development (from **V1.2.0-eo**, `pico/defines.h`).

**Branch:** `1.1.x` — periodically merge or fast-forward from `main` when the maintenance line should pick up a newer snapshot (see session log). Push with `git push -u origin 1.1.x` when credentials allow.

**Typical workflow:**

1. **Patch the 1.1.x line:** `git checkout 1.1.x` → edit → `make -C cpanel` and `cmake --build pico/pico_release` (and `pico2_release`) as usual → commit on `1.1.x` → `git push origin 1.1.x`.
2. **Return to tip of `main`:** `git checkout main` (and `git pull` if collaborating).
3. **Optional:** Cherry-pick a fix from `1.1.x` onto `main`, or merge `main` into `1.1.x` only when you intentionally bring `main` changes into the maintenance line.

**Note:** Uncommitted work in the working tree is visible on whichever branch is checked out; **commit** release snapshots on `1.1.x` so the branch records the exact tree (e.g. V1.1.24-eo sources and `pico/_releases/V1.1.24-eo/` artifacts you care to track).

---

## 11. Summary table of code locations

| Topic | Key files | Decision / fix |
|-------|-----------|----------------|
| U2 address range | `defines.h`, `busloop.c` | C0x4–C0x7 only; no GPIO slot select |
| ip65 W5100 probe + SHAR | `uthernet2.c` §`u2_reset`, §1c | RTR $07/$D0 + RMSR/TMSR $06 for `w5100.s` probe; default SHAR = `00:08:DC:A2:A2:A2` (matches `w5100.s`) so RMSR=$06 short path does not leave `cfg_mac` all-zero |
| Two DATA reads both $07 | §1f, `uthernet2.c` `auto_increment` | Pointer only advances when **MR** has **AI** ($02); **$03** = IND+AI; **$00** after reset or **$01** → no increment, second read still RTR0 |
| First `$C0C7` wrong then RTR OK | §1f, `busloop.c`, `U2_PeekDataPort` | RP2350 PIO prefetches next read’s byte; **`r[7]`** must hold **`U2_PeekDataPort()`** after each U2 cycle so first DATA read after addr setup isn’t stale |
| **`async_context` PANIC at `ck=5`** | §1g, `main.c`, `u2_monitor.h` | **`U2_MonPollFlush`** only on **core 0** — not from **`U2_Poll`** on core 1 |
| MACRAW RX `drop (no room)` / DHCP stall | §1g, `uthernet2.c` `u2_socket_discard_rx` | On overflow, discard unread RX (RX_RD→wr) then accept new frame |
| UART vs “Device not found” | §1c, `debug/*.log` | `w5100.s` `init` only `SEC`s on RTR XOR; correct RTR reads ⇒ that run passed Ethernet init; **`ip65_init` then `clc`s unconditionally** — see §1c if UI still says device not found; **48× `DATA read`** after `mode=0x03` traces RMSR/SHAR/OPEN |
| UART boot identity | `main.c`, `build_id.h.in`, `CMakeLists.txt` | After reboot, scroll to **latest** `Megaflash DEBUG Firmware…` block: includes **`FIRMWAREVERSTR`** and **`Firmware build:`** (UTC + Unix s from `build-both.sh`/`cmakeall.sh`; **`unknown` / `0`** if configured without timestamp) |
| ip65 init bisect (Pico 2 W Debug) | `CMakeLists.txt` `U2_IP65_CHECKPOINT`, `uthernet2.c`, `u2_monitor.c` | Default **quiet**: one **`[u2] ck=n`** per run when **`U2_IP65_CHECKPOINT=n`** (1=MODE 0x03 … 5=MACRAW OPEN). Optional **`U2_IP65_TRACE_DATA`**, **`U2_MON_LOG_BUS`** (floods UART) |
| Apple readback for U2 | `busloop.c` U2 branch, `a2bus.h` | **`registers.r[4..7]`** → **`i32[1]`** chunk **1** → **`UpdateMegaFlashRegisters(1,…)`**; RP2350 waits for IRQ0 before update (§1d) so SM1 presents the merged byte on the next cycle |
| U2 read data path | `busloop.c`, `a2bus.h` | U2 read byte must update **chunk 1** (SM1), not chunk 0 |
| RP2350 U2 vs PIO IRQ 0 | `busloop.c` §1d | U2 branch must wait for IRQ 0 clear before `UpdateMegaFlashRegisters(1,…)` (same as main loop); skipping caused bad C0C4–C0C7 reads |
| U2 `[u2m]` monitor | `u2_monitor.c`, `build-debug.sh` §1e | Debug-only queued UART trace; flush from `U2_Poll`; bus + socket + net hooks |
| A0–A3, nDEVSEL pulls | `a2bus_rp2040.pio`, `a2bus_rp2350.pio` | A2=GPIO8, A3=GPIO9, no pulls; nDEVSEL pull-up on; data bus pull-up |
| Build SDK path | `cmakeall.sh`, `CMakeLists.txt` | Script passes `-DPICO_SDK_PATH`; `CMakeLists.txt` uses **`$HOME/pico-sdk`** when env or `-D` unset (§4); SDK is same git repo on all host architectures |
| Host CMake | `build-env.sh`, `cmakeall.sh`, `build-both.sh`, `build-debug.sh` | **`CMAKE_BIN`**: `/opt/homebrew/bin/cmake` → `/usr/local/bin/cmake` → **`PATH`**; **`CMAKE`** env override |
| Pico-capable GCC | `build-env.sh` `mf_try_arm_toolchain_bin`, `cmakeall.sh`, `build-both.sh` | Must run on host CPU **and** resolve existing **`nosys.specs`** (rejects Homebrew bare GCC + Intel **.pkg** on Apple Silicon); else scripts **`exit 1`** with install hint |
| Host **picotool** | `pico/picotool/picotool/picotool`, `picotool-src` | Must match host CPU; rebuild from **`picotool-src`** + **`cmake --install`** to **`pico/picotool/`** if link fails (**§4b**). **libusb:** **`brew install libusb`** (+ **`pkgconf`**) on Apple Silicon for **arm64** dylib; else **`PICOTOOL_NO_LIBUSB=1`** for UF2-only |
| **cpanel / Java** | `cpanel/Makefile`, **`tools/register-arm-openjdk-macos.sh`** | **`make all`** needs **arm64** **`java`**; register Homebrew JDK in **`JavaVirtualMachines`** (**§4c**) or **`JAVA_HOME`**; Intel **`/usr/local/Cellar/openjdk`** removed manually if Intel **`brew` fails |
| **vs ThomasFok upstream** | **`ThomasFok-upstream-comparison.md`** (root), §4d–**§4h** | **§4e–§4f:** Pico storage/DMA + **`dmamemops`**. **§4g:** **`smartport.s`**. **§4h:** **`fswrts`** + **`swjmp_ay`**, **`B1_FFC8`**. **Not merged:** Thomas **`HOMESEGMENT`** all of **`megaflash.s`**, **`flash.h`** rename, **TFTP DOS-order** |
| Version bump | `cmakeall.sh`, `defines.h` | Grep with trailing space; `awk '{print $3}'`; `tr -d '\r\n'`; string = "Vx.y.z-eo"; **1.2.x** = `V1.2.0-eo` / `0x0020` onward |
| Release output | `cmakeall.sh` | Build then copy UF2s to `_releases/<NEW_VER>/` |
| Both-board test build | `build-both.sh` | `pico_release` + `pico2_release` (Release), cpanel first; no `defines.h` bump; passes **`FIRMWARE_BUILD_TIMESTAMP`** (Unix s) into CMake each run |
| Firmware build timestamp | `build-both.sh`, `cmakeall.sh`, `CMakeLists.txt`, `build_id.h.in` | `-DFIRMWARE_BUILD_TIMESTAMP` + `-DFIRMWARE_BUILD_TIMESTAMP_STR` → generated **`build_id.h`** (Unix + UTC string); **`CMD_GETFIRMWAREVER`** / **`DoGetDeviceInfo`** bytes **[12..15]** LE; USB string shows readable time + Unix s, or **`__DATE__`/`__TIME__`** if unset |
| Debug behaviour | `main.c`, `debug.h`, `lwipopts.h` | Debug = UART + logs + bus loop always; Release = no UART, no logs; as of 1.1.20 both always run bus loop and core0Loop when CheckPicoW() (see §7b) |
| Release testing | §7b, §7c | Test Release build (NTP/TFTP/WiFi) before shipping; use pre-release checklist (§7c) to avoid Debug-only validation |
| C0C4 diagnostic | `busloop.c` | LED on 1 s on any $C0C4 access; non-blocking |
| nDEVSEL sense | Both PIO files | Active-low (trigger on low); inverted sense was tried and reverted |
| TFTP/UDP performance | `udptask.h`, `udptask.cpp` | HEARTBEAT_PERIOD 50→10 ms; 50 ms added latency per packet; blocking flash erase also stalls loop (see §13) |
| TFTP OOM panic at start | §13d, `misc.c`, `network_pump.cpp`, `ramdisk.c` | Panic is **pico_malloc** (`new` for ~2.5 KiB); **CP “RAM disk off”** does not free **`ramdisk_data[]`**; **`DebugPrintHeapState("NETPUMP: TFTP pre-new")`** before TFTP `new` |
| TFTP OOM panic at start | §13b, `misc.c`, `network_pump.cpp`, `ramdisk.c` | Panic is **pico_malloc** (`new` for ~2.5 KiB); **CP “RAM disk off”** does not free **`ramdisk_data[]`**; **`DebugPrintHeapState("NETPUMP: TFTP pre-new")`** before TFTP `new` |
| TFTP hostname default | `busloop.c`, `cpanel/tftp.c` | After command that sets data buffer, push chunk 0 to PIO immediately (§7g); clear hostname if it looks like status text |
| TFTP upload block count UI | `tftptxtask.cpp`, `tftprxtask.cpp` | Set `tftp_state.tsize` (TX) / WiFi status (RX) in `EvtStart()`; pump path does not call `Run()` (§7h) |
| CP version + clock | `cpanel/asm-megaflash.s` | `CMD_GETFIRMWAREVER` → cols 20–31; `CMD_GETTIMESTR` → 32–39; `ClearTime` clears 20–39 (§10c) |
| Flash JEDEC at boot | `flash.c` `ChipIDToCapacity` | §16: capacity from type+capacity bytes only; manufacturer byte ignored |
| Flash validate (Applesoft) | `tools/flash-validate/` | §17-18: `FLASHVAL.BAS` baseline + `FLASHSOAK.BAS` overnight CSV/TFTP loop; `build-flashval-disk.sh` → `FLASHVALID.po` |
| Drives Enable toggles | `cpanel/drivesenable.c` | `gotoxy` Y is WNDTOP-relative; do not add `YPOS` (§10d) |
| Git 1.1.x patches | branch `1.1.x` | `checkout 1.1.x` to patch/build; `checkout main` to resume tip (§10e) |
| NetworkPump entry | `network_pump.cpp`, `network.cpp`, `main.c` | `RunNTP` / `RunTestWifi` / `RunTFTP` register a short-lived `LegacyUdpSessionAdapter` and spin `PollOnce()` until `GetCompleted()`; `CUDPTask::Run()` still wraps `EnterRunSession` + same loop for any direct caller; Core 0 idle `NetworkPump_PollOnce` (§14.8) |
| lwIP DNS/UDP vs `runningObject` | `udptask.cpp`, `network_pump.{h,cpp}` | DNS: `dns_pending_owner_` (`INetworkSession*`) + `OnDnsGetHostByNameResult` (§14.11), with pending-owner armed before `dns_gethostbyname` to avoid fast-callback race/timeouts. UDP: `NetworkPump_LegacyUdpRecv` + pcb→`INetworkSession*` (`udp_pcb_owners_`); `OnUdpRecvPbuf(pcb,p,…)` → `NotifyUdpReceived` or U2 (§14.10, §14.10b) |
| Uthernet II lwIP | `uthernet2_net.{h,cpp}` | Pump: `AddSession`, `CreateUdpPcb`, `PollOnce`; TCP: `U2TcpArg` + `u2_tcp_*` callbacks (§14.10b) |
| Pump TCP + session timers | `network_pump.{h,cpp}` | `CreateTcpPcb`: `tcp_arg(owner)`, `NetworkPump_LegacyTcpRecv` / `NetworkPump_LegacyTcpErr` → `OnTcpRecvPbuf` / `OnTcpErr`; `tcp_pcb_owners_` for unregister. `ScheduleTimer` / `CancelTimer`; `PollOnce` → `DrainSessionTimers` → `OnTimer` (§14.12) |

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

## 13d. TFTP start: `*** PANIC ***` / `Out of memory`

**Symptom:** Enabling TFTP download panics immediately (log shows `NETPUMP: RunTFTP …` then panic), not mid-transfer.

**Why (root cause):** The message is from **Pico SDK** `pico_malloc` (`pico-sdk/.../pico_malloc/malloc.c`): `panic("Out of memory")` when **`malloc`/`new` returns NULL** or when **`(ptr + size)` crosses `__StackLimit`** (heap/stack collision). TFTP constructs **`CTFTPRXTask`** / **`CTFTPTXTask`**, whose bases allocate **`CUDPTask::rxbuffer`** (~1500 B, `UDP_BUFFERSIZE`) and **`CTFTPTask::txbuffer`** (~`TXBUFFERSIZE`, order of 1 KiB) — roughly **2.5 KiB** of **heap** in successive allocations. If **free heap** at that moment is near or below that (e.g. boot prints ~2.7–3.9 KiB free in a tight configuration), **`new` fails** and the firmware panics.

**RAM disk “disabled” in CP does not shrink RAM use:** `ramdisk.c` reserves **`static uint8_t ramdisk_data[RAMDISK_SIZE]`** (e.g. 256 KiB on RP2350 as of `defines.h`). **Runtime disable** only skips using it as a volume; the **BSS buffer stays allocated**. To reclaim space you need a **build-time** change (smaller `RAMDISK_SIZE`, or conditional compile to omit the array when the product does not need RAM disk).

**What we did:** **`DebugPrintHeapState()`** in `misc.c` / `misc.h` logs **`heap_region`**, **`free`**, and **`mallinfo`** (`arena`, `uordblks`, `fordblks`) under tag **`NETPUMP: TFTP pre-new`**, immediately before **`new CTFTPRXTask` / `new CTFTPTXTask`** in `NetworkPump::RunTFTP` (`network_pump.cpp`). Use the UART line to confirm **free < ~3 KiB** at TFTP start.

**Further diagnosis (optional):** Enable **PICO_DEBUG_MALLOC** (Pico SDK `PICO_CONFIG`) so the malloc wrapper **prints failing allocation sizes** to `printf` — e.g. **`target_compile_definitions(... PRIVATE PICO_DEBUG_MALLOC=1)`** on the firmware target, or configure-time **`-DPICO_DEBUG_MALLOC=1`** if your SDK picks it up. Inspect **`.map`** / **picotool** for total SRAM layout; compare **`lwipopts.h`** `MEM_SIZE` (lwIP pool, separate from C heap) vs **OOM** — this panic is **C heap / `new`**, not lwIP `MEM_SIZE` unless the same failure path triggers elsewhere.

**References:** `pico/misc.c` (`GetTotalHeap` / `GetFreeHeap` / `DebugPrintHeapState`), `pico/network_pump.cpp` (`RunTFTP`), `pico/udptask.cpp` (`CUDPTask` ctor), `pico/tftptask.cpp` (`CTFTPTask` ctor), `pico/ramdisk.c` (`ramdisk_data`), `pico-sdk/.../pico_malloc/malloc.c`.

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
- **DNS callbacks:** **`arg = INetworkSession*`** (issuer) plus **`dns_pending_owner_`** for stale detection (§14.11). A pump-only **`void*`** without the session pointer cannot disambiguate late completions.
- **TCP callbacks:** For **`CreateTcpPcb`**-owned pcbs, **`tcp_recv`/`tcp_err`** dispatch to **`INetworkSession`** (§14.12). Full **`tcp_sent`/`tcp_poll`/accept** wiring remains per-session when Uthernet (or others) move off ad-hoc lwIP hooks.
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

**Stage 3 status (NTP, Test WiFi, TFTP + shared pump plumbing):** All three Apple-facing flows go through **`NetworkPump`** for **legacy-operation bookkeeping**. **`CUDPTask`** is split into **`BeginRun()`** (WiFi + UDP pcb), **`StartEventsAfterBeginRun()`** (watchdog + **`EvtStart()`**), and **`PumpNetworkIteration()`** (one loop iteration: poll, DNS/UDP/timer, wait). **`CUDPTask::Run()`** still chains **`EnterRunSession()`** + those pieces + a **`while (PumpNetworkIteration())`** loop. **`NetworkPump::RunNTP` / `RunTestWifi` / `RunTFTP`** instead register a short-lived **`LegacyUdpSessionAdapter`** (`INetworkSession::OnPump` → one **`PumpNetworkIteration()`** per **`PollOnce()`**) and spin **`PollOnce()`** until **`GetCompleted()`**—same work per iteration as the inner **`Run()`** loop, but the legacy task is now a **registered pump session** so the architecture matches future multi-session **`OnPump`** use.

| Entry point | Pump method | Notes |
|-------------|-------------|--------|
| `GetNetworkTime()` | `NetworkPump::RunNTP` | `LEGACY_OPERATION_NTP`; `CNTPTask` via `EnterRunSession` + `BeginRun` + `StartEventsAfterBeginRun` + adapter + `PollOnce` loop; epoch on success for `InitRTC`. |
| `TestWifi()` | `NetworkPump::RunTestWifi` | `LEGACY_OPERATION_TESTWIFI`; same pattern; exception → `NetworkError_t` mapping in pump. |
| `ExecuteTFTP()` | `NetworkPump::RunTFTP` | `LEGACY_OPERATION_TFTP`; RX/TX tasks use the same pump-driven pattern (no `CUDPTask::Run()` from the pump). |

**`NetworkPump::PollOnce()`** calls **`cyw43_arch_poll()`** then **`OnPump()`** on each registered **`INetworkSession`** (max 8). During a legacy NTP/TFTP/Test WiFi operation, **`LegacyUdpSessionAdapter`** is on the list so each **`PollOnce()`** advances one **`PumpNetworkIteration()`** (plus an extra top-level **`cyw43_arch_poll()`** before **`OnPump`**). **`core0Loop`** calls **`NetworkPump_PollOnce()`** during the idle wait between NTP retries / IPC messages so lwIP gets extra polls when no legacy task is blocking Core 0. **`INetworkSession`** methods have **default empty** implementations; **`OnPump`** is the hook for future async work.

**`NetworkPump::RequestAbortAll()`** calls **`Abort()`** on every registered session, then **`UDPTask_RequestAbortIfRunning()`**.

**Takeaway:** One **`CUDPTask`** is still the active **`runningObject`** at a time for NTP/TFTP/Test WiFi (serialized **`runningObject`**). DNS pending owner is **`INetworkSession*`** on **`NetworkPump`** (§14.11); UDP receive is **`NetworkPump`-dispatched** via pcb→**`INetworkSession*`** (§14.10). **Uthernet II** uses **`CreateUdpPcb`**, shared **`PollOnce`**, and **`Uthernet2Session::Abort`** (§14.10b); U2 TCP uses custom **`U2TcpArg`** recv/err rather than **`CreateTcpPcb`**. Further work: **`RequestDNS(hostname, …)`** wrapper; optional **`tcp_sent`/`tcp_poll`** for U2.

### 14.9 Legacy callback routing (incremental)

**Goal:** Stop using **`CUDPTask::runningObject`** as the gate for **lwIP** DNS callbacks so the stack can move toward **pump-dispatched** events and (later) **multiple** concurrent clients.

**Evolution:** §14.9 first used a file-static **`s_dnsPendingTask`** in **`udptask.cpp`**. That is superseded by **`NetworkPump::dns_pending_owner_`** (**`INetworkSession*`**) and **`NetworkPump_LegacyDnsCallback`** (§14.11); behaviour (stale callback drop, clear on result/timeout/**`LeaveRunSession`**) is unchanged.

**References:** `pico/udptask.cpp` (`DNSLookup`, `PumpNetworkIteration`, `LeaveRunSession`, `OnDnsGetHostByNameResult`); `pico/network_pump.{h,cpp}`.

### 14.10 Single UDP recv path via `NetworkPump` (pcb → `INetworkSession*`)

**Goal:** One lwIP **`udp_recv_fn`** for all pump-registered UDP traffic, with **`arg = &GetNetworkPump()`**, matching §14.4’s “pump looks up session by pcb”.

**Mechanics:**
- **`INetworkSession::OnUdpRecvPbuf(udp_pcb*, pbuf, addr, port)`** — default no-op; **`pcb`** disambiguates when one session owns multiple UDP sockets (Uthernet II). **`CUDPTask`** ignores **`pcb`** and **`NotifyUdpReceived`** as before.
- **`CUDPTask`** **inherits** **`INetworkSession`** and overrides **`OnUdpRecvPbuf`** to **`NotifyUdpReceived`** (same **`pbuf`** copy path as the old **`udp_callback`** body).
- **`CUDPTask::BeginRun`** uses **`GetNetworkPump().CreateUdpPcb(this, 0)`** (ephemeral port): **`udp_new`**, **`udp_bind`**, **`RegisterUdpPcbOwner`**, **`udp_recv(..., NetworkPump_LegacyUdpRecv, pump)`**. Non-**`CUDPTask`** sessions use the same API with their own **`INetworkSession`**.
- **`~CUDPTask`** calls **`DestroyUdpPcb(pcb)`** (**`UnregisterUdpPcb`** + **`udp_remove`**).
- **`NetworkPump::dispatchLegacyUdpRecv`** looks up **`INetworkSession*`** in **`udp_pcb_owners_`**, then **`OnUdpRecvPbuf(pcb, p, …)`**; frees **`pbuf`** after return.
- **`GetNetworkPump()`** / **`g_networkPump`** live **outside** **`extern "C"`** in **`network.cpp`** so the function has C++ linkage.

**References:** `pico/network_pump.{h,cpp}`, `pico/network.{h,cpp}`, `pico/udptask.{h,cpp}`.

### 14.10b Uthernet II (`uthernet2_net.cpp`) on the pump

**Requirement:** Drive U2 lwIP traffic through the same **`PollOnce`** path as NTP/TFTP/Test WiFi; register UDP with **`CreateUdpPcb`**; **`RequestAbortAll`** should tear down U2 sockets via **`INetworkSession::Abort`**.

**What we did:**
- **`Uthernet2Session`** implements **`OnUdpRecvPbuf`** (match **`udp_pcb*`** to emulated socket index) and **`Abort()`** (**`U2_Net_Close`** all slots). **`U2_Net_Init`** calls **`GetNetworkPump().AddSession(&g_u2_session)`** once (duplicate add is ignored by **`AddSession`**).
- **UDP:** **`CreateUdpPcb` / `DestroyUdpPcb`**; **`U2_Net_Poll`** → **`NetworkPump_PollOnce()`** (no direct **`cyw43_arch_poll`** in U2).
- **TCP:** Not **`CreateTcpPcb`** / **`NetworkPump_LegacyTcpRecv`**: lwIP **`tcp_err`** does not pass **`tcp_pcb*`**, so **`tcp_arg`** holds **`U2TcpArg { sock_index }`** and **`u2_tcp_recv` / `u2_tcp_err`** (C linkage) handle recv/err. **`RegisterTcpPcbOwner`** still records **`&g_u2_session`** per pcb. **`pcb->callback_arg`** is read before **`tcp_arg(pcb, nullptr)`** in teardown (**`tcp_arg`** is setter-only in this lwIP).
- **Listen:** Before **`tcp_listen_with_backlog`**, **`u2_release_tcp_arg`** frees the client pcb’s **`U2TcpArg`**; the returned listen pcb is **`u2_attach_tcp_pcb`** + **`tcp_accept(u2_tcp_accept_cb)`**.

**What we did not do:** Wrap **`PollOnce`** in **`cyw43_arch_lwip_begin/end`** (Core 1 **`U2_Net_Poll`** vs Core 0 idle **`PollOnce`** — monitor if stack requires mutex around lwIP from both cores).

**References:** `pico/uthernet2_net.{h,cpp}`, `pico/network_pump.{h,cpp}`.

### 14.11 DNS callback + pending task on `NetworkPump`

**Goal:** Centralize legacy DNS “who is waiting?” state next to the UDP pcb map, without **`static`** state in **`udptask.cpp`**, while preserving correct **stale-callback** behaviour.

**Why `arg` is the issuing session:** **`dns_gethostbyname`** passes a single **`void*`** to the callback. lwIP does not give a per-request handle other than that **`arg`**. If **`arg`** were only **`&GetNetworkPump()`**, a **late** DNS completion for lookup **A** could be applied to session **B** after **`RegisterDnsPendingOwner(B)`** overwrote the single pending slot—**wrong**. The fix is: **`arg = static_cast<void*>(static_cast<INetworkSession*>(issuer))`** **and** **`session == dns_pending_owner_`** in the callback (superseded lookup or session ended clears the slot or replaces it).

**Mechanics:**
- **`DNSLookup`:** **`RegisterDnsPendingOwner(this)`** is armed **before** **`dns_gethostbyname(..., NetworkPump_LegacyDnsCallback, void_ptr_to_INetworkSession)`** so a fast callback cannot race and be dropped. On immediate **`ERR_OK`** / **`ERR_ARG`** / other immediate failure, clear pending owner right away; on **`ERR_INPROGRESS`** keep the owner set and wait for callback/timeout. This fixes intermittent DNS timeouts caused by callback-before-registration ordering.
- **`NetworkPump_LegacyDnsCallback`:** If **`!IsDnsPendingOwner(session)`**, ignore (stale). Else **`session->OnDnsGetHostByNameResult(ipaddr)`** (**`nullptr`** **ipaddr** = invalid host). **`CUDPTask`** sets **`dnsCallbackInvoked`**, **`dns_error`**, **`dns_result_ipaddr`** for **`EvtDNSResult`**.
- **`PumpNetworkIteration`** / DNS timeout / **`LeaveRunSession`:** **`ClearDnsPendingOwner(this)`** when appropriate (same clear points as the old **`s_dnsPendingTask`**).

**What we did not do:** A single **`RequestDNS(hostname, timeout_ms)`** API on the pump that also owns **`dnsTimeout`** / **`EvtDNSResult`** delivery—still **`CUDPTask::DNSLookup`** + **`PumpNetworkIteration`** for legacy tasks.

**References:** `pico/network_pump.{h,cpp}`, `pico/udptask.{h,cpp}`.

### 14.12 TCP pcb helpers + pump session timers

**Goal:** Match §14.4’s TCP routing sketch and unblock future **`INetworkSession`** users (e.g. Uthernet II) without each protocol installing raw lwIP callbacks.

**TCP (`CreateTcpPcb` / `DestroyTcpPcb`):**
- **`tcp_new`**, **`RegisterTcpPcbOwner(pcb, owner)`**, **`tcp_arg(pcb, owner)`**, **`tcp_recv`/`tcp_err`** → **`NetworkPump_LegacyTcpRecv`** / **`NetworkPump_LegacyTcpErr`**.
- **`INetworkSession::OnTcpRecvPbuf`** default frees **`pbuf`** if non-NULL; **`OnTcpErr`** is default no-op. lwIP’s **`tcp_err`** callback does **not** pass **`tcp_pcb*`** — sessions with multiple TCP sockets must track which pcb failed internally.
- **`DestroyTcpPcb`** clears callbacks, **`tcp_abort`**, unregisters from **`tcp_pcb_owners_`**. Code that **replaces** **`tcp_recv`** after **`CreateTcpPcb`** (e.g. **`tcp_listen`**, **`tcp_accept`**) must keep **`tcp_arg`** ownership consistent with the pump.

**Session timers (`ScheduleTimer` / `CancelTimer`):**
- One **slot per** **`INetworkSession`** (new **`ScheduleTimer`** replaces the previous deadline for that session).
- **`PollOnce`** runs **`DrainSessionTimers()`** after **`OnPump`**; expired timers call **`OnTimer(arg)`**.
- **`RequestAbortAll`** clears pending pump timers (legacy **`CUDPTask`** timers unchanged).

**What we did not do:** Use **`CreateTcpPcb`** for Uthernet II TCP (see §14.10b — custom **`tcp_arg`**); additional **`tcp_sent`/`tcp_poll`** wiring if needed later.

**References:** `pico/network_pump.{h,cpp}`; Uthernet path §14.10b.

---

## 15. Control Panel TFTP host/file swap safeguard

**Symptom:** When launching TFTP upload from the Control Panel after a TestWifi run, the Pico debug log showed the host and filename arriving swapped or stale, e.g. hostname = the Pico's IP and filename = the typed host.

**Why this was treated as a Control Panel issue:** The Pico-side `CMD_TFTPRUN` parser reads the first NUL-terminated string as hostname and the second as filename. Debug logs showed the Pico receiving wrong values already in `tftp_state`, so the bad state was being sent from the Apple side before the Pico parsed it.

**What we changed:** In `cpanel/tftp.c`, before copying the strings into the MegaFlash data buffer, we now:

- Check whether the filename looks like an IPv4 address.
- If the filename looks like an IP address while the hostname does not, swap the two values.
- Show a warning on the TFTP screen (`"Warning: host/file looked swapped"`).

This is intentionally conservative: the common case remains unchanged, but the control panel now catches the exact swapped-host/file pattern observed during debug and avoids sending an obviously wrong TFTP request.

**References:** `cpanel/tftp.c`, `cpanel/ui-textinput.c`, `pico/cmdhandler.c`, `pico/network.cpp`.

---

## 16. Flash boot: JEDEC capacity without manufacturer check

**What:** `InitFlash()` only enables storage when `ChipIDToCapacity(tsReadJEDECID(...))` is non-zero; previously that function matched the full 24-bit ID, which fixed the manufacturer byte to Winbond (`0xEF`).

**Why:** Drop-in SPI NOR parts from other vendors (e.g. Alliance) can use the same memory-type and capacity bytes as W25Q*JV but a different JEP106 manufacturer ID, and were incorrectly rejected at boot.

**What we did:** `ChipIDToCapacity()` in `pico/flash.c` now compares **`id & 0xFFFF`** (memory type + capacity code) to the same supported pairs as before (`0x4020`/`0x7020` → 64 MB, `0x4021`/`0x7021` → 128 MB, `0x7022` → 256 MB). **`tsReadJEDECID()`** returns **`id & 0xFFFFFF`** so only the 24-bit RDID is used. An explicit **`jedec24 == 0x204020`** branch documents **Alliance Memory** 512 Mbit (same class as W25Q512JV); that ID also matches the **`type_cap`** path.

**What we did not do:** SFDP-based detection for unrelated ID layouts; guarding `SetFlashDriveStrength()` when SR3 layout differs — operators should verify Alliance/other datasheets match Winbond-style commands and status registers, or extend firmware if not.

**References:** `pico/flash.c` (`ChipIDToCapacity`, `InitFlash`, `SetFlashDriveStrength`).

---

## 17. Applesoft flash-path validator (`tools/flash-validate`)

**What:** An Applesoft program (`FLASHVAL.BAS`) drives the same **`$C0C0`–`$C0C3`** command/parameter/data path documented in `cpanel/asm-megaflash.s`, runs non-destructive **`CMD_*`** calls that hit the Pico flash stack (`CMD_GETDEVINFO`, `CMD_GETDIB`, `CMD_READBLOCK`, etc.), and can save or compare a **text baseline** of 16-bit checksums.

**Why:** Validates end-to-end behaviour (Apple ↔ bus interface ↔ firmware ↔ SPI flash) without requiring the Control Panel binary; useful when swapping flash vendors or firmware builds.

**What we did:** Added `tools/flash-validate/FLASHVAL.BAS` and `README.md` (file format `FLASHVAL1`, slot base formula, volatile fields). **`build-flashval-disk.sh`** builds a standard **143360-byte ProDOS 140K** **`FLASHVALID.po`** using the same mechanism as **`../a2speed/Makefile`** (`-pro140`, then **`-p` / `-bas` / `-ptx`**). **PRODOS** and **BASIC.SYSTEM** are copied with **`-g`** from **`cpanel/prodos19.dsk`** (known-good image in-tree), not from padding **`pico/romdisk.po`** to 800K (that produced non-standard images some tools reject). **`Makefile`** in **`tools/flash-validate/`** mirrors **`make disk`** entry points (Homebrew **`java`**, default **`AppleCommander-ac.jar`**). **`-bas`** adds tokenized **FLASHVAL** from **`FLASHVAL.DSK.BAS`** (screen-only suite), **`-ptx`** adds **`FLASHVAL.SRC`**. Port variables must not be named **`PR`**: Applesoft treats **`PR`** as **`PRINT`**, and AppleCommander’s bastools fails with **`Expecting: [PR, #]`**; use **`PX`** (param port) and **`D1`** (data port) instead.

**What we didn’t do:** Destructive tests (`CMD_FORMATDISK`, `CMD_ERASEDISK`, `CMD_WRITEBLOCK`); those need explicit write-enable key handling and should stay a separate tool. Full **`FLASHVAL.BAS`** is not reliably **`-bas`**-tokenized (file I/O and tokenizer quirks); disk boot program is **`FLASHVAL.DSK.BAS`**.

**References:** `tools/flash-validate/README.md`, `tools/flash-validate/build-flashval-disk.sh`, `tools/flash-validate/Makefile`, `tools/flash-validate/FLASHVAL.BAS`, `tools/flash-validate/FLASHVAL.DSK.BAS`, `cpanel/prodos19.dsk`, `common/defines.h`, `pico/cmdhandler.c`.

---

## 18. Overnight volume/TFTP soak validator (`tools/flash-validate/FLASHSOAK.BAS`)

**What:** Added a second Applesoft validator, **`FLASHSOAK.BAS`**, designed for unattended overnight cycles with CSV logging and destructive media verification. It exercises each writable MegaFlash unit (including RAM disk when enabled) with a repeatable sequence: format, file workload (create/append/delete/fill), whole-volume checksum, TFTP upload to `192.168.0.10` as `validationX.po`, reformat, TFTP download, checksum compare, and cycle-level pass/fail logging.

**Why:** `FLASHVAL` validates command-path integrity and selected reads, but not sustained write/format/file/TFTP churn. The soak tool targets long-duration reliability and data-integrity regressions across flash + RAM media and network image round trips.

**What we did:** Implemented command wrappers in Applesoft for `CMD_GETUNITSTATUS` (`0x12`), `CMD_FORMATDISK` (`0x1D`), `CMD_READBLOCK` (`0x15`), `CMD_TFTPRUN` (`0x50`), and `CMD_TFTPSTATUS` (`0x51`) with firmware-compatible parameter ordering (including write-enable key `0x71` and null-terminated hostname/filename in data buffer). Added CSV event logging (`cycle,unit,event,result,v1,v2`) and repeat-when-pass loop semantics. The disk build adds tokenized **`FLASHSOAK`** and **`FLASHSOAK.SRC`** to the same **140K** **`FLASHVALID.po`** as §17 (see **`build-flashval-disk.sh`** / **`Makefile`**).

**What we didn’t do:** This pass does not include an external host-side parser/aggregator for CSV statistics; the output is intentionally plain CSV for downstream tooling. We also did not attempt to make this non-destructive.

**References:** `tools/flash-validate/FLASHSOAK.BAS`, `tools/flash-validate/README.md`, `tools/flash-validate/build-flashval-disk.sh`, `pico/cmdhandler.c` (`DoFormatDisk`, `DoReadBlock`, `DoTFTPRun`, `DoTFTPStatus`), `common/defines.h`.

*This document reflects reasoning and changes made during development; it may be extended as further design decisions are documented.*

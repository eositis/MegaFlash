# Implementation Notes and Reasoning

This document records the thinking, root-cause analysis, design decisions, and dead ends from work on the MegaFlash Pico firmware (Uthernet II, build, GPIO, C0C4 visibility). It is intended for future maintainers and for debugging similar issues.

---

## Chronology and context (from session work)

- **Version series:** **V1.1.24-eo** is the last **1.1.x** maintenance release. Ongoing work targets **1.2.x** starting from **V1.2.0-eo** (`0x0020` in `pico/defines.h`), focused on **Uthernet II emulation**, **com port**, and **imagewriter emulation**.
- **ip65 / Uthernet II:** U2 emulation was adapted so the ip65 stack (no changes to ip65) works: RECV command advances RX_RD to sn_rx_wr; socket CR is cleared to 0 after each command; default RMSR/TMSR = 0x06; MACRAW RX is fed by wrapping netif->input when socket 0 is opened in MACRAW. U2 debug logging uses prefix `[u2]` and is gated by UTHERNET2_DEBUG (Debug build only), not NDEBUG. See `docs/ip65-Uthernet-II-integration.md`.
- **Uthernet II**: Confirmed U2 at $C0C4–$C0C7 only; no GPIO slot select. Fixed read-back of Mode Register (chunk 1 vs chunk 0). (Earlier: C0C4 diagnostic LED — **removed** in 1.2.x; see §9.)
- **GPIO pulls**: A2/A3 pulldowns disabled (bus-driven). nDEVSEL pull-up first disabled at user request, then re-enabled when C0C4 was not seen; with pull-up enabled, C0C4 still not recognized.
- **Build**: PICO_SDK_PATH added to cmakeall.sh; version bump + “-eo” + date on each build; version-bump script fixed (grep uniqueness, strip newlines); release UF2s copied to `_releases/<version>/`.
- **C0C4 not seen**: Logic analyzer shows A0–A4 (and presumably address) at the Pico, but firmware never sees the access (no response). Conclusion: PIO only pushes a cycle when **nDEVSEL (GPIO 20) goes low**; if it does not go low for that access, the CPU never gets the cycle. Tried inverting nDEVSEL sense (trigger on pin HIGH = DEVSEL active-high); **reverted** at user request. nDEVSEL sense remains active-low.
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

**What we did:** Before `UpdateMegaFlashRegisters(1, …)` inside the U2 branch, added the same `pio_interrupt_get` wait as the main loop (guarded with `#ifndef PICO_RP2040`). Initially this mirrored the main loop’s `pio_sm_is_rx_fifo_empty` guard; that guard was **removed on the U2 path only** so IRQ 0 always clears before updating chunk 1 — skipping the wait when the RX FIFO was non-empty could still leave **$C0C4–$C0C7** stale and break the ip65 RTR XOR (wget65 / telnet65 “device not found” with slot 4 correct).

**Also:** `write_common_register()` in `uthernet2.c` now stores **RTR / RCR / PTIMER** and other **$0001–$002F** gaps that were previously no-ops on write (real W5100 retains those bytes).

**RP2350 store visibility:** `UpdateMegaFlashRegisters()` (`pico/a2bus.h`) uses **`pio0->rxf_putget[SM_A2BUS][chunk] = value`**. Added **`__dmb()`** after that store so the **PIO** sees the new chunk word before the next **`mov osr, rxfifo[y]`**. Symptom without it: **UART** `[u2] DATA read` shows correct **$0017/$0018** bytes (emulator path) but **only two** trace lines and **wget** “no device found” — **6502** `eor` XOR still fails because the **bus** latched a **stale** FIFO byte (same class of issue as §1f).

**PIO timing (pull earlier):** `a2bus` originally used **long** side-delays (`[7]`…`[6]`) before **`mov osr, rxfifo[y]`** so the CPU had time after the previous cycle — but that also **delayed** the first **`out pins`** into the slot **read** window. If the **6502** samples the data bus before the PIO drives the correct byte, **`w5100.s`** sees **garbage** while a **later** C snapshot matches emulation. **Tighter** pre-FIFO delays (`[3]`…`[1]`) aim to present the byte **earlier**; **IRQ 0** still blocks **`UpdateMegaFlashRegisters`** until the SM has pulled the **current** cycle’s FIFO value. **`__dsb`** after **`__dmb`** on **`rxf_putget`** write strengthens store completion before the SM can pull.

**U2 hot path in SRAM (RP2350):** Between IRQ 0 clear and **`UpdateMegaFlashRegisters(1,…)`**, only **CPU cycles** remain. **`U2_HandleBusAccess`**, **`U2_PeekDataPort`**, **`read_value` / `read_value_at`**, **`write_*`**, **`auto_increment`**, **`write_common_register`**, **`set_rx_sizes` / `set_tx_sizes`** are placed in **`.time_critical.*`** (**`__time_critical_func`**) so they execute from **RAM**, not **XIP flash** — fewer wait states on every **`$C0C4–$C0C7`** access. **`read_socket_register`** (socket block) stays in flash; ip65 **RTR** path uses **common** memory via **`read_value_at`** only.

**References:** `pico/busloop.c`, `pico/a2bus_rp2350.pio` (`irq set 0` / `mov osr,rxfifo[y]` / `irq clear 0`), `pico/a2bus.h`, `pico/uthernet2.c` (`write_common_register`).

---

## 1c. telnet65 “Device not found” vs ip65 W5100 `init` (MegaFlash U2)

**What:** `ip65/apps/telnet65.s` calls `ip65_init` with `A = eth_init_default` (`ip65/drivers/a2init.s`, slot **4** for MegaFlash). On failure it prints **“Device not found”** (`ip65_init` returns carry).

**Why:** `ip65_init` → `eth_init` → Contiki/ip65 **`drivers/w5100.s`** `init`. The **only** path that returns **`SEC`** before socket setup is the **RTR fingerprint** at addresses **$0017 / $0018**: two reads with XOR must produce zero (default **$07,$D0**). Any wrong values (open bus, wrong slot, or bad emulation) → probe fails → “Device not found.” After that, if **RMSR == $06**, ip65 **skips** the software-reset block that **writes SHAR** but still **reads SHAR back** into the driver MAC and then `ethernetcombo.s` copies that into **`cfg_mac`**. With SHAR all zeros, **station MAC is invalid** (DHCP/ARP issues) even though the probe passed.

**What we did:** `u2_reset()` already matched **RTR/RMSR/TMSR** for the probe; added default **SHAR** = same bytes as **`w5100.s`** (`00:08:DC:A2:A2:A2`) so the short init path gets a sane MAC.

**What we didn’t do:** Cannot fix “no device” from firmware if the CPU never addresses **slot 4** `$C0C4–$C0C7` or **nDEVSEL** does not assert (see §2b/§12).

**References:** `ip65/ip65/ip65.s` (`ip65_init`), `ip65/drivers/ethernetcombo.s` (`eth_init`/`init_adaptor`), `ip65/drivers/w5100.s` (`init` through RTR XOR and RMSR branch), `ip65/apps/telnet65.s`, `pico/uthernet2.c` (`u2_reset`).

**UART symptom:** Only one `[u2] mode=0x03…` line then “Device not found” is **normal** for `U2_DEBUGF` volume — the RTR probe uses **ADDRHI/ADDRLO + DATA** reads, which did not log. **Debug** builds also print **`RTR data read addr=0x0017 -> …`** / **`0x0018`** (first 8 such reads per boot) so captures show whether the 6502 reached the probe and got **0x07** / **0xD0**. If those lines never appear, bus cycles are not completing to the DATA port; if values are wrong, XOR fails and `eth_init` returns carry.

**Reconciling UART with the screen:** In stock **`ip65/drivers/w5100.s`**, the **only** **`sec` / `rts`** in **`init`** is the **RTR XOR** failure; after OPEN, **`init` ends with `clc` / `rts`**. **`ip65_init`** only branches to the device-failure path on **`eth_init` carry**; if **`eth_init` clears carry**, **`ip65_init` always ends with `clc` / `rts`** (it unconditionally **`clc`** after `timer_init` / `arp_init` / `ip_init`). So a capture that shows **correct RTR** (`0x0017`/`0x0018`) for the **same** telnet65 attempt **contradicts** a stock **`“Device not found”`** from **`ip65_init`** unless the Apple disk is **not** stock ip65, there is **carry corruption** (extraordinary), or the on-screen line is being **misattributed** to the same UART window. **MegaFlash policy:** do **not** patch ip65 for Uthernet II — fix behaviour in **`pico/`** emulation only so stock ip65 remains the reference.

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

## 1h. WGET65V — forked `wget65` with on-screen W5100 handshake trace (`tools/`)

**What:** A local fork at **`tools/wget65-verbose/`** (upstream **`ip65/apps/wget65.c`**, **`w5100.c`**, **`w5100_http.c`**, **`linenoise`**, plus **`wget65_verbose_regs.c`**) logs each Ethernet bring-up step to the **Apple II screen**: **`ip65_init`** (stock **`w5100.s`** RTR XOR inside), **`w5100_init`**, **`dhcp_init`** (if not DNS-offload), **`w5100_config`**. After each step it prints MR, RTR0/RTR1 with the same **XOR chain** as **`w5100.s`** (`$D7 ^ RTR0 ^ RTR1` should be `$00`), RMSR, and PTIMER (DNS offload hint).

**Why:** Complements UART **`[u2]`** traces on the Pico: operators can capture **what the 6502 sees** on the bus after **`ip65_init`** without a serial cable.

**What we did:** Switched **`#include "../inc/ip65.h"`** to **`<ip65.h>`** with **`-I$(IP65_ROOT)/inc`** so the fork builds standalone next to an unmodified **`ip65`** checkout. **`wget65.c`** enables **`videomode(VIDEOMODE_80COL)`** on **`__APPLE2__`** (same pattern as **`hfs65.c`**) so verbose lines fit on screen. **`eth_init`** is fixed to **slot 4** (**`$C0C4`**) via **`WGET65V_ETHER_SLOT`**; **`ethernet.slot`** is not read (stock **`wget65`** still uses it). **`build-flashval-disk.sh`** adds **`WGET65V`**, **`WGET65V.SYSTEM`**, launcher **`WGET65V`**, and **`WGET65V.DOC`** when **`tools/wget65-verbose/wget65v.bin`** exists (default **`WGET65V_BIN`** = **`../wget65-verbose/wget65v.bin`** from **`flash-validate/`**) and **`cl65 --print-target-path`** yields **`apple2enh/util/loader.system`**; otherwise only **`WGET65V.DOC`** is added. **`wget65v.bin`** is **gitignored**. Top-level **`ip65/`** clone (build dependency) is **gitignored**.

**What we didn’t do:** No edits to upstream **`ip65`** in-repo (policy §1c).

**References:** `tools/wget65-verbose/README.md`, `tools/flash-validate/README.md`, `tools/flash-validate/build-flashval-disk.sh`.

**WGET65V screenshot paradox:** `ip65_init` fails (**`ip65_error` = `$85`** = device failure) while the **post-failure** register dump shows **MR=`$03`**, **RTR XOR chain = `$00`**, **RMSR=`$06`** — i.e. values that **would** satisfy **`w5100.s`** if read **then**. That is **consistent** with §1c §1d: the **probe** inside **`eth_init`** may see **stale or wrong** bytes on the **6502** bus while **`read_value()`** / a **later** C snapshot matches emulation. **WGET65V** prints **`ip65_error`** and an on-screen note after Step A fails so the dump is not misread as “chip OK but ip65 wrong.”

---

## 1i. AppleWin Uthernet II vs MegaFlash (timing and handshake)

**What:** Use **AppleWin** as a **behavioural** reference for W5100/Uthernet II (registers, RTR defaults, port layout), not as a **bus-timing** simulator.

**Why (AppleWin handshake):** In **`Uthernet2::IO_C0`** (`AppleWin` `source/Uthernet2.cpp`), the card is invoked when the emulated CPU performs the slot I/O cycle. The return value is computed **in the same emulated step** — no separate listener FIFO, no PIO prefetch, no IRQ 0 ordering window.

- **Reads:** `BYTE res = write ? 0 : MemReadFloatingBus(nCycles);` then **`switch (loc)`** overwrites **`res`** with **MR / address high / address low / data** (`readValue()`, etc.). **`MemReadFloatingBus`** (`source/Memory.cpp`) supplies the Apple IIe **floating-bus** pattern for realism; the **final** byte the 6502 “sees” for a successful decode is still the emulated W5100 value, not an analog of Pico setup/hold.
- **Port index:** `loc = address & U2_C0X_MASK` (**`0x03`**) — only **A0/A1** of the slot I/O address select which of the four ports (mode, addr hi, addr lo, data) is used. That matches **`U2_HandleBusAccess`**, which uses **`busdata & U2_C0X_MASK`** (`pico/w5100_regs.h`).

**What differs on MegaFlash (hardware):**

1. **Same PIO read path for storage and U2:** On RP2350, **one** `a2bus` state machine serves **all** slot nibbles; **`y` = A3:A2** only picks **which 32‑bit `rxf_putget` word** is shifted out. The **`wait` / side-delay / `out pins, 8`** sequence that positions data vs **PHI0** / **nDEVSEL** does **not** branch on “U2 vs MegaFlash.” So if **$C0C0–$C0C3** reads are reliably correct on the bench, there is **no separate “U2 Φ2”** that ought to differ from **storage Φ2** — any **electrical** setup/hold problem would likely hurt **both** chunks (or the whole card), not only **$C0C4–$C0C7**.

2. **Where U2 *does* differ — software pipeline, not a second bus schedule:** Before **`UpdateMegaFlashRegisters(1, …)`**, the U2 branch runs **`U2_HandleBusAccess`**, **`U2_PeekDataPort`**, IRQ 0 wait, etc. (§1d, §1f). That is **extra CPU work and ordering** between “this bus cycle was sampled” and “chunk 1 written for the **next** SM pull.” AppleWin has **none** of that. Symptoms like **RTR XOR fails** while internal state looks right point here or at **stale prefetched `r[7]`**, not at “U2 needs a different oscilloscope alignment than ProDOS block reads” **unless** storage reads are also flaky on the same hardware.

3. **Address range:** **`busloop.c`** enters U2 only for **`addr` 4–7** (`$C0C4–$C0C7`). On real Uthernet II, the W5100 front-end often decodes **only A0/A1**, so **`$C0C8`** (nibble **8**) is the **same logical port** as **`$C0C4`** (mode) — see AppleWin issue/PR discussion around **`IO_C0`** and **`0x03`**. MegaFlash **reserves** `$C0C8–$C0CF` for future **ACIA** (§1b), so those nibbles are **not** U2 mirrors here; behaviour can **diverge** from AppleWin if any code touches mirrored slot addresses.
4. **PIO chunk = A3:A2:** SM1 selects **`rxf_putget[y]`** with **`y` from A3:A2**. Nibbles **8–B** and **C–F** map to **chunks 2–3**, not chunk 1. Full AppleWin-style mirroring would require **routing** more nibbles through **`U2_HandleBusAccess`** **and** either **replicating** chunk‑1 words into chunks **2–3** on each U2 update or **changing** the PIO address→chunk mapping — not a one-line change.

**Takeaway:** AppleWin confirms **what** the W5100 stack expects to read/write; it does **not** model **when** the data bus is valid on real metal. **Φ2 / scope work** is for **validating the card’s analog timing** if reads are suspect **across** addresses — not because U2 is on a **different** physical read schedule than **$C0C0–$C0C3**. Prefer debugging U2-specific issues on the **pipeline** (chunk 1, IRQ 0, **`U2_PeekDataPort`**, handler cost). For decode parity, decide product policy on **`$C0C8+`** mirroring vs ACIA reservation.

**References:** [AppleWin `Uthernet2.cpp`](https://github.com/AppleWin/AppleWin/blob/master/source/Uthernet2.cpp) (`IO_C0`, `Reset` / RTR defaults), [AppleWin `Memory.cpp`](https://github.com/AppleWin/AppleWin/blob/master/source/Memory.cpp) (`MemReadFloatingBus`); `pico/busloop.c`, `pico/uthernet2.c`, `pico/a2bus_rp2350.pio`, §1b §1d §1f.

---

## 1j. ip65 DHCP over MACRAW — align station MAC with CYW43 (wget65 / telnet65 “stuck obtaining IP”)

**What:** After **`eth_init`** succeeds, **`dhcp_init()`** (ip65) sends **DHCP DISCOVER/REQUEST** as **raw Ethernet** via **W5100 MACRAW** (socket 0 OPEN). MegaFlash **`U2_Net_SendMacraw`** passes the frame to **`netif->linkoutput`** (`cyw43_send_ethernet`) after aligning **MAC** fields with the **CYW43 STA** (below).

**Why:** **`u2_reset()`** defaulted **SHAR** to the same **WIZnet OUI** as stock **`w5100.s`** (`00:08:DC:…`). **Ethernet source MAC** in frames from the II therefore **did not** match the **CYW43 STA** **`netif->hwaddr`**. Many APs only accept **802.11** frames whose **Ethernet SA** is the **associated STA MAC**. Separately, **BOOTP/DHCP** carries a **client hardware address** (**`chaddr`**, bytes **28–33** of the BOOTP message inside the **UDP** payload). Servers often **unicast** **OFFER/ACK** using **`chaddr`**; if that field still held the **WIZnet** MAC while we only fixed **Ethernet SA**, the **UDP** payload could be **inconsistent** and **checksum-invalid** after **`chaddr`** changes — leading to **DHCP timeout** even when **DISCOVER** left the **WiFi** side.

**What we did:** On **`U2_Net_OpenMacraw(0)`**, copy **`netif_default->hwaddr`** into **`u2_memory[W5100_SHAR0..5]`** via **`U2_SetStationMacFromBytes()`** so ip65 reads **SHAR** consistent with the **radio**. On **every** **`U2_Net_SendMacraw`**: (1) for **IPv4** **UDP** **sport 68** / **dport 67**, **BOOTREQUEST** (**op** **=** **1**), patch **`chaddr`** to **`netif->hwaddr`**, **zero** the **UDP** checksum field, recompute with **`lwip`** **`inet_chksum_pseudo`** over the **UDP** datagram (same **IPv4** **src/dst** as in the frame); (2) overwrite **Ethernet** **SA** (bytes **6–11**) with **`netif->hwaddr`**; (3) call **`U2_SetStationMacFromBytes`** again so emulated **SHAR** stays aligned if **`OPEN`** ran before **`netif`** was ready.

**What we didn’t do:** No edits to upstream **ip65**.

**STA netif vs `netif_default` / `netif_list` (DHCP + TCP/80):** In **`cyw43_lwip`**, **`netif_set_default(n)`** runs for **each** interface; the **last** one initialized wins. If **AP** (**`w1`**) is up, **`netif_default`** can point at **AP**, while **station** traffic must use **`cyw43_state.netif[CYW43_ITF_STA]`** (**`w0`**). **`cyw43_netif_output`** derives **`itf`** from **`netif->name[1] - '0'`**, so sending via the wrong **`struct netif`** transmits on the **wrong** CYW43 interface. **`U2_Net_SendMacraw`**, **SHAR** alignment, and the **MACRAW** **`input`** hook must use the **STA** netif **explicitly**, not **`netif_default`** or **`netif_list`** (head may not be STA). **TCP CONNECT:** **`tcp_bind`** to **Sn_PORT** before **`tcp_connect`** so the emulated client matches **W5100** local-port semantics.

**Packet tracing (not tcpdump):** There is **no** pcap/tcpdump on the Pico itself without capturing to flash/USB. **Off-device:** mirror the AP port or use a hub/tap and **`tcpdump`/`wireshark`** on a PC. **SDK (unused by default):** **`CYW43_NETUTILS`** + **`cyw43_state.trace_flags`** (**`CYW43_TRACE_ETH_TX`/`RX`**) in **`cyw43_lwip.c`** — requires rebuilding **`cyw43-driver`** with **`CYW43_NETUTILS`**. **MegaFlash UART:** **`U2_ETH_HEADER_TRACE=1`** (**CMake** cache or **`U2_ETH_HEADER_TRACE=1 ./build-debug-both.sh`**) wraps **STA** **`input`**/**`linkoutput`** and prints **`[u2eth]`** **TX**/**RX** plus **first 64 bytes** hex per frame (**Ethernet + start of IPv4/TCP/UDP**); **`U2_Net_Poll`** installs the chain once **CYW43** is initialized (**before** **MACRAW** **OPEN** is the normal order).

**References:** `pico/uthernet2.c` (`U2_SetStationMacFromBytes`, `U2_Net_ConnectTcpEx` + **Sn_PORT**), `pico/uthernet2_net.cpp` (`u2_cyw43_sta_netif`, `U2_Net_OpenMacraw` / `SendMacraw` / `Close`, `u2_macraw_patch_dhcp_bootp_chaddr`), `pico/uthernet2.h`, `lwip` `inet_chksum_pseudo`, `cyw43_lwip.c` (`netif_set_default`, `cyw43_netif_output`).

---

## 1k. Debug crash with `[u2eth]`: core affinity for network poll

**What:** Debug capture (`debug/uart_log.txt`) showed `*** PANIC *** async_context_poll context check failed (IRQ or wrong core)` near MACRAW SEND while `[u2eth]` trace was active.

**Why (root cause):**

- `busloop.c` calls `U2_Poll()` from the Apple bus path (core 1).
- `U2_Poll()` called `U2_Net_Poll()`, and `U2_Net_Poll()` called `NetworkPump_PollOnce()`.
- `NetworkPump_PollOnce()` touches CYW43/lwIP poll context, which must run on the owning core (core 0 in this firmware design).
- Under traffic, this cross-core poll path trips the SDK async-context guard (`wrong core`).

**What we did:**

- In `pico/uthernet2_net.cpp`, `U2_Net_Poll()` now returns immediately unless `get_core_num() == 0`.
- In `pico/main.c`, `PicoW_ServiceCore0IpcAndNetwork()` now calls `U2_Net_Poll()` (core-0 path), so the same function performs both optional `[u2eth]` hook install and network pump polling on the correct core.
- Kept `U2_Poll()` callable from core 1, but it no longer drives network poll off-core.
- Follow-up from a fresh `uart_log` capture: panic still reproduced on first MACRAW SEND because `U2_Net_SendMacraw()` itself called `cyw43_arch_lwip_begin/end` from bus/core 1. Fixed by deferring MACRAW TX to core 0:
  - `U2_Net_SendMacraw()` now queues frame bytes when called off-core.
  - `U2_Net_Poll()` (core 0) drains one pending MACRAW frame and performs `linkoutput` there.
  - Result: CYW43/lwIP lock entry for MACRAW TX is core-0-only.

**What we didn’t do:** No protocol-level changes to U2 MACRAW/TCP/UDP behavior; this is execution-context routing only.

**Takeaway:** U2 bus logic can run on core 1, but CYW43/lwIP poll and transmit entrypoints must stay core-0-only. Keep any future U2 network calls that touch `cyw43_arch_lwip_*` behind core-0 dispatch.

**References:** `pico/uthernet2.c` (`U2_Poll`), `pico/uthernet2_net.cpp` (`U2_Net_Poll`), `pico/main.c` (`PicoW_ServiceCore0IpcAndNetwork`), `debug/uart_log.txt`.

---

## 1l. DHCP timeout follow-up: MACRAW TX pointer wrap handling

**What:** After fixing wrong-core panics (§1k), DHCP still timed out. `[u2eth] TX` showed good DHCP frames intermittently, but also suspicious large/zero-heavy frames and truncated-looking payload starts. A follow-up change attempted to alter TX pointer arithmetic in `send_data()`.

**Why this change regressed behavior:**

- The attempted fix computed `data_len` from **full 16-bit** `Sn_TX_RD/Sn_TX_WR`, while the rest of this emulator path (`get_tx_data_size`/`TX_FSR`) uses **masked ring-pointer** semantics.
- That made SEND length computation inconsistent with free-space/reporting logic and changed how `Sn_TX_RD` low/high bytes were advanced after SEND.
- In bench testing this caused Uthernet emulation to stop responding correctly for ip65 init flows (“device not found” class behavior), i.e. regression worse than the original DHCP symptom.

**What we did:**

- Reverted `send_data()` back to the prior masked-ring math:
  - `rd = Sn_TX_RD & mask`, `wr = Sn_TX_WR & mask`
  - `data_len = wr - rd; if (data_len < 0) data_len += buf_size`
  - advance `Sn_TX_RD` using that ring-space value path
- Rebuilt Debug firmware (`./pico/build-debug-both.sh`) and confirmed the build is clean.

**What we didn’t do:** No changes to DHCP patching, RX hook chain, or MACRAW core-affinity queueing from §1j/§1k in this regression rollback.

**Takeaway:** In this codebase, TX pointer handling must stay internally consistent across SEND and TX_FSR paths. Mixing full-pointer math in one path with masked-ring math in another is unsafe and can break basic ip65 detection/bring-up.

**References:** `pico/uthernet2.c` (`send_data`), `debug/uart_log.txt` (`[u2eth]` malformed/large TX observations).

---

## 1m. MACRAW shifted/zero TX frames: `Sn_TX_RD` pointer collapse on SEND

**What:** With `[u2tap]` enabled in `uthernet2_net.cpp`, malformed MACRAW TX packets (`len=257` shifted, `len=514` zero-heavy) were traced to already-corrupted payload at queue drain (`drain`) before any core-0 DHCP/SA patching.

**Why (root cause):**

- `send_data()` used masked ring pointers for copying payload (`rd = Sn_TX_RD & mask`, `wr = Sn_TX_WR & mask`) — correct for ring indexing.
- But it also advanced `Sn_TX_RD` registers using the **masked** `wr`, not full host-visible `Sn_TX_WR`.
- Over successive SENDs this collapses high-byte pointer progression, so later reads start at wrong offsets (or stale areas), producing shifted/zero MACRAW frames.

**What we did:**

- Kept masked pointers for ring read math only.
- Preserved full register progression by writing `Sn_TX_RD := Sn_TX_WR(full)` (`wr_full` high/low bytes) after SEND in `pico/uthernet2.c`.
- Rebuilt with low-noise trace profile and kept `[u2tap]` active for immediate validation.

**What we didn’t do:** No protocol-path changes in `U2_Net_SendMacraw`; corruption occurs before that stage.

**Takeaway:** For W5100 emulation, ring-address masking and register-pointer progression are separate concerns; masking must not leak into `Sn_TX_RD` register updates.

**References:** `pico/uthernet2.c` (`send_data`), `pico/uthernet2_net.cpp` (`[u2tap]` stages), `debug/uart_log.txt` (`drain/core0-pre/core0-post` correlation).

---

## 1n. Root cause found: TX pointer register reads returned RX_RSR bytes

**What:** Pointer traces showed host-side `Sn_TX_WR` progression `0x0124 -> 0x0225 -> 0x0427`, producing malformed `len=257/514` MACRAW sends (shifted headers, zero payload blocks).

**Why (root cause):**

- In `read_socket_register()` (`pico/uthernet2.c`), cases for `W5100_SN_TX_RD0/1` and `W5100_SN_TX_WR0/1` were incorrectly grouped with `W5100_SN_RX_RSR0`, returning `get_rx_rsr_byte(..., 8)` instead of actual register bytes.
- Any host driver read of TX RD/WR thus received RX-queue-derived values, corrupting host pointer math and subsequent TX frame construction.

**What we did:**

- Fixed `read_socket_register()` so `W5100_SN_TX_RD0/1` and `W5100_SN_TX_WR0/1` return `u2_memory[address]` (actual emulated register contents).
- Kept `W5100_SN_RX_RSR0/1` handling unchanged (`get_rx_rsr_byte`).
- Rebuilt with trace profile to verify behavior after this correction.

**What we didn’t do:** No further protocol changes in core-0 net path; this is a register-read correctness fix in the W5100 emulation layer.

**Takeaway:** Correct register decode in socket-register reads is critical; mixing dynamic status register handlers with pointer register handlers can silently poison host-side pointer arithmetic.

**References:** `pico/uthernet2.c` (`read_socket_register`, `send_data`), `debug/uart_log.txt` (`MACRAW ptrs` + `[u2tap]` sequences).

---

## 1o. DHCP offers visible but ip65 timeout: MACRAW RX length field compatibility

**What:** After fixing TX pointer decode (§1n), logs showed stable outbound DHCP DISCOVER (`68->67`) and inbound OFFER/ACK traffic (`67->68`, `len=342`) reaching MACRAW RX (`net sock0 MACRAW rx len=342`), yet ip65 still timed out.

**Why (likely compatibility mismatch):**

- `u2_push_rx_macraw()` wrote the 2-byte MACRAW length field as raw frame length (`len`).
- Common W5100 MACRAW drivers read that field as `wire_len` and then subtract 2 before consuming payload bytes.
- If firmware writes `len` instead of `len+2`, the client reads `len-2` bytes (truncated frames), which can make DHCP parsing fail even when packets arrive.

**What we did:**

- Updated `u2_push_rx_macraw()` in `pico/uthernet2.c` to write `wire_len = len + 2` in the MACRAW length prefix while keeping payload bytes unchanged.
- Rebuilt Debug trace firmware for validation.

**What we didn’t do:** No changes to netif RX hook or socket RECV command flow in this step.

**Takeaway:** In MACRAW mode, length-prefix wire contract matters as much as payload; off-by-2 in prefix can look like “DHCP timeout” despite correct network ingress.

**References:** `pico/uthernet2.c` (`u2_push_rx_macraw`), `debug/uart_log.txt` (`[u2eth] RX 67->68`, `MACRAW rx len=342`).

---

## 1p. ADTPro `restart system-$01`: latest log shows no W5100 activity before crash

**What:** After the DHCP/MACRAW fixes, ADTPro still reports `restart system-$01`. In the latest capture (`Firmware build: 2026-04-27 01:00:59 UTC`), the run window contains only ambient LAN RX noise and no U2 socket events.

**Why (current best diagnosis):**

- The log has no `sock0 OPEN`, `SEND`, `RECV`, or ip65 checkpoint events during the failing run.
- This suggests ADTPro crashes before (or without) issuing the expected W5100 init/access sequence, so the current failure is not yet attributable to malformed MACRAW frames on the firmware side.
- Prior TX-pointer and MACRAW length fixes remain valid for ip65 traffic, but they do not explain this specific crash signature.

**What we did:**

- Re-validated the latest `debug/uart_log.txt` tail against expected U2 markers and confirmed absence of W5100 command traffic at failure time.
- Built a targeted bisect debug image with `U2_IP65_CHECKPOINT=1` (`U2_ETH_HEADER_TRACE=1 U2_IP65_TRACE_DATA=0 U2_MON_LOG_BUS=0`) to test whether ADTPro reaches the first mode write (`MR=0x03`) at all.

**What we didn’t do:** No new emulation behavior change was applied in this step; this is diagnostic narrowing only.

**Takeaway:** Before changing packet-path logic again, confirm ADTPro reaches U2/W5100 init on the failing run; otherwise the crash likely occurs earlier in ADTPro startup/driver handoff.

**References:** `debug/uart_log.txt`, `pico/build-debug-both.sh`, `pico/uthernet2.c` (ip65 checkpoints), `pico/u2_monitor.c`.

---

## 1q. Temporary ADTPro workaround: force `COMMSLOT` to slot 4 (skip auto-scan)

**What:** To avoid early `adtproeth` crashes before any observed W5100 activity, we applied a temporary client-side workaround in the local ADTPro tree to force Uthernet slot 4 and bypass slot auto-discovery.

**Why (rationale):**

- Startup path in ADTPro Ethernet calls `PARMDFT` before network use; when `CONFIGYET==0`, `PARMDFT` invokes `FindSlot`.
- `FindSlot` probes multiple slots via `ip65_init`, and this can fail/crash before reaching the intended MegaFlash Uthernet slot.
- For MegaFlash, Uthernet is fixed at slot 4 (`$C0C4-$C0C7`), so scanning is unnecessary for current testing.

**What we did:**

- Edited local ADTPro file: `/Users/eositis/Documents/GitHub/adtpro/src/client/prodos/ethernet/ethconfig.asm`.
- In `PARMDFT`, replaced `FindSlot` path with forced assignment `COMMSLOT=3` (zero-based slot 4) and `DEFAULT=3`.
- Updated default table entries so restore/reset paths keep slot 4 (`COMMSLOT` and `DEFAULT` from 2 to 3).
- Built client via Ant with explicit host paths: `JAVA_HOME=/opt/homebrew/opt/openjdk ant -DassemblerPath=/opt/homebrew/bin prodos-ethernet`.
- Ant failed later in packaging due missing custom `appleDump` task in this environment, but compile/link succeeded and produced:
  - `/Users/eositis/Documents/GitHub/adtpro/src/client/ADTPROETH.BIN`
  - `/Users/eositis/Documents/GitHub/adtpro/src/client/adtproeth.map`

**What we didn’t do:** We did not alter MegaFlash firmware behavior in this step; this is strictly a temporary ADTPro client workaround for isolation.

**Takeaway:** For fixed-slot hardware targets, bypassing ADTPro slot scan can isolate startup crashes from network emulation issues and allow focused runtime validation.

**References:** `adtpro/src/client/prodos/ethernet/ethconfig.asm` (local clone), `adtpro/build/build.xml` (`prodos-ethernet` target), generated `ADTPROETH.BIN`.

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

**Symptom:** Logic analyzer shows the C0C4 access (e.g. address/data) at the Pico, but the firmware never sees it: Uthernet II does not respond (no U2 handler activity).

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

## 7i. Pico W: WiFi LED off + Control Panel Test WiFi hang when not in `core0Loop()`

**Symptom:** CYW43 WiFi LED never comes on; **Test WiFi** from the Control Panel appears to hang (until timeout).

**Root cause:** On Pico W, **`core0Loop()`** (NTP, **`NetworkPump_PollOnce()`**, **`multicore_fifo`** handling for **`IPCCMD_WIFITEST`** and **`IPCCMD_TFTP`**) runs only when **`IsAppleConnected()`** is true at boot, or after a later 2 s poll detects the Apple. If **`appleConnected`** is false, the firmware takes the **USB / `UserTerminal()`** path instead. That path **never** popped the FIFO or polled lwIP/CYW43, so **`DoTestWifi()`** on core 1 pushed an IPC message and spun on **`testResult.testCompleted`** while core 0 never ran **`TestWifi()`**. Same starvation for TFTP IPC. A plain **`sleep_ms(1000)`** when USB was not connected also left the stack idle for up to one second between polls.

**What we did:** Factor **`PicoW_ServiceCore0IpcAndNetwork(fifo_timeout_us)`** (same order as before: optional FIFO wait, then **`NetworkPump_PollOnce()`**, **`U2_MonPollFlush()`**, then dispatch). **`core0Loop()`** inner loop calls it with **50 ms** FIFO timeout. The Pico W USB-terminal **`while (true)`** calls it with **0** each iteration (non-blocking) and replaces the 1 s idle sleep with **1 ms** sleeps so **`cyw43_arch_poll`** runs continuously during idle.

**What we didn’t do:** **`UserTerminal()`** can still block core 0 for a long time; Test WiFi from the Apple while an interactive USB session is active may still stall until the terminal returns. Unusual vs Apple-only + no USB.

**References:** `pico/main.c` (`PicoW_ServiceCore0IpcAndNetwork`, `core0Loop`, Pico W branch), `pico/cmdhandler.c` (`DoTestWifi`, `multicore_fifo_push_timeout_us`).

---

## 7j. Release: USB connected ↔ Apple II bus inactive (Debug build exempt)

**Requirement:** In **Release** (`NDEBUG`), when a **USB host** is connected, the Pico must **not** emulate/respond on the Apple II bus; when the **Apple II bus** is in use (Apple connected), **USB serial terminal** must be **disabled**. **Debug** builds (no `NDEBUG`) keep the previous behaviour: UART + USB + bus can all be exercised for development.

**What we did:**
- **`g_release_bus_emulation_enabled`** = `IsAppleConnected() && !stdio_usb_connected()` (Release only). **`ReleaseInitBusUsbGate(apple_at_boot)`** before **`multicore_launch_core1`**; **`ReleaseUpdateBusUsbGate()`** from core 0 (main loop and **`core0Loop`** inner wait) — refreshes on USB edge or every 250 ms for PHI0 reconnect.
- **`a2bus.h`**: **`GetAppleBusBlocking()`** in Release polls **`pio_sm_get_rx_fifo_level`** when emulation is enabled so the gate can take effect without waiting for a bus cycle; when emulation is off, spins on **`g_release_bus_emulation_enabled`** without reading the FIFO.
- **`main.c`**: **`stdio_usb_init()`** / **`InitPicoLed()`** run **before** **`core0Loop()`** on Pico W so **`stdio_usb_connected()`** is meaningful in the network loop. **`UserTerminal()`** only if **`stdio_usb_connected() && !IsAppleConnected()`** (Release).

**Trade-offs:** If **both** USB and Apple are connected, both gate conditions force **bus off** and **terminal off** until one is unplugged. Hot-unplug detection can lag up to **250 ms** for Apple-only changes. Release **idle** with bus enabled uses a **busy** wait on empty FIFO (slightly higher CPU than **`pio_sm_get_blocking`**) so USB can preempt without a slot access.

**References:** `pico/misc.c`, `pico/misc.h`, `pico/a2bus.h`, `pico/main.c`.

---

## 7k. WiFi dead / Test WiFi timeout: double `cyw43_arch_init` after `InitPicoLed`

**Symptom:** CYW43 WiFi LED never comes on; Control Panel Test WiFi runs until **timeout**; NTP/TFTP similarly broken.

**Root cause:** **`InitPicoLed()`** (`misc.c`) calls **`cyw43_arch_init()`** so the Pico W LED can use **`cyw43_arch_gpio_put`**. Later, **`CUDPTask::InitCyw43()`** and **`NetworkPump::Init()`** called **`cyw43_arch_init_with_country()`**, which (in pico-sdk) sets country and calls **`cyw43_arch_init()`** again. **`cyw43_arch_init()`** always invokes **`cyw43_driver_init()`**; a second init is not idempotent. On failure the SDK **deinitializes** (`cyw43_arch_deinit()` in `cyw43_arch_poll.c`), leaving the stack torn down after the first successful init.

**What we did:** Before calling **`cyw43_arch_init_with_country()`**, if **`cyw43_is_initialized(&cyw43_state)`** (already inited by `InitPicoLed`), only call **`cyw43_arch_enable_sta_mode()`** and turn on the WL LED — **no** second init.

**References:** `pico/udptask.cpp` (`InitCyw43`), `pico/network_pump.cpp` (`NetworkPump::Init`), `pico/misc.c` (`InitPicoLed`), pico-sdk `src/rp2_common/pico_cyw43_arch/cyw43_arch_poll.c`, `cyw43_arch_poll.c` (`cyw43_arch_init` / `cyw43_driver_init`).

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

## 9. C0C4 diagnostic LED (removed)

**Historical:** For a time, `busloop.c` turned on the activity LED (ACT_LED_PIN) for 1 s on any **`$C0C4`** access to confirm the firmware saw U2 Mode Register traffic. **Removed** to keep the core1 bus loop lean (no `pico/time` / GPIO on the U2 path).

**Debugging:** Use UART **`[u2]`** / **`[u2m]`** (Debug), logic analyzer on **nDEVSEL** + address, or §2b — **not** the LED.

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

## 10f. Telnet65 logical walkthrough and follow-up fixes

**What:** Perform a code walkthrough of Uthernet II emulation and logically test a Telnet65 flow (ip65 init/DHCP via MACRAW, then TCP connect/send/recv) to identify likely regressions before changing firmware.

**Why (findings):**

1. **TX path empty-buffer bug risk:** `send_data()` treats `TX_WR == TX_RD` as full buffer because it uses `if (data_len <= 0) data_len += buf_size;` (should only wrap on negative). That can cause a SEND on empty TX to transmit stale full-buffer data.
2. **pbuf chain handling risk (UDP/TCP RX):** U2 RX paths use `p->payload` with `p->tot_len` directly, but lwIP pbufs can be chained. For chained pbufs, this can read past first-segment payload or copy invalid data.
3. **RECV semantics are intentionally lossy:** SN_CR=RECV forces `RX_RD = sn_rx_wr` (discard unread). This was added for ip65 progress, but it can drop unread bytes if host software does partial reads or if multiple frames queue up between polls.
4. **Potential nested lwIP lock in UDP open:** `U2_Net_OpenUdp()` wraps `CreateUdpPcb()` with `cyw43_arch_lwip_begin/end`, while `CreateUdpPcb()` already takes the same lock. This is a re-entrancy/lock-order hazard depending on SDK lock behavior.

**What we did:**

- Fixed TX empty-send wrap in `send_data()` by wrapping only when `wr-rd` is negative (not zero), so `TX_WR == TX_RD` stays empty.
- Flattened UDP/TCP lwIP pbufs with `pbuf_copy_partial()` before calling `push_rx_cb`, so chained pbufs are handled safely.
- Removed the outer `cyw43_arch_lwip_begin/end` in `U2_Net_OpenUdp()`; `CreateUdpPcb()` already owns that lock.
- Rebuilt both release targets with `./pico/build-both.sh` (RP2040 + RP2350) successfully.

**Takeaway:** Telnet65-critical send/receive paths now better match lwIP and W5100 expectations, reducing risk of stale TX sends and malformed RX data under chained-pbuf traffic.

**References:** `pico/uthernet2.c` (`send_data`, `SN_CR_RECV`, `u2_push_rx`), `pico/uthernet2_net.cpp` (`OnUdpRecvPbuf`, `u2_tcp_recv`, `U2_Net_OpenUdp`), `pico/network_pump.cpp` (`CreateUdpPcb`).

---

## 10g. Other ip65 tools walkthrough (`date65`, `tweet65`, `hfs65`, `wget65`)

**What:** Review the remaining ip65 Apple II tools for compatibility with current Uthernet II emulation, using each tool's socket behavior.

**Scope mapped from ip65 apps:**

- `date65`: `ip65_init` + DHCP + DNS + SNTP (primarily UDP paths in ip65 stack).
- `tweet65`: `ip65_init` + DHCP + HTTP-trigger path (TCP via ip65 tcp library).
- `hfs65`: `ip65_init` + DHCP + HTTP server (`httpd_start`, TCP listen/accept/send).
- `wget65`: custom W5100 shared-access client (`apps/w5100.c`, `apps/w5100_http.c`) using socket 1 with partial `receive_commit()` / `send_commit()` patterns.

**Why (findings):**

1. **High: RECV command is too aggressive for shared-access clients (notably `wget65`)**  
   In emulation, SN_CR=RECV forces `RX_RD = sn_rx_wr` (discard all unread RX).  
   ip65 `apps/w5100.c` expects W5100 semantics where host-updated RX_RD is honored and only committed bytes are consumed. `wget65` often commits partial chunks (`w5100_receive_commit(rcv)`), so unread tail bytes can be dropped.

2. **High: TCP RX overflow can silently drop data while still ACKing lwIP input**  
   `u2_push_rx()` drops frame when W5100 RX ring is full, but `u2_tcp_recv()` still calls `tcp_recved()` unconditionally after push attempt. This can acknowledge bytes to peer even when not delivered to emulated W5100 memory.  
   Risk is highest for bulk-transfer tools (`wget65`, `hfs65` large responses), lower for interactive `telnet65`.

3. **Medium: SEND path caps one command to 2048 bytes**  
   `send_data()` copies into local fixed 2048-byte buffer for TCP/UDP sends.  
   With ip65 shared-access (`w5100.c`) and socket memory layouts that allow >2KB queued data per SEND, payload may be truncated per SEND command.

4. **Low: date65 path remains comparatively robust**  
   `date65` relies mostly on DHCP/DNS/SNTP (UDP + MACRAW init path), which align with current known-good telnet init path and are less exposed to partial RECV semantics.

**Takeaway:**  
`date65` is likely to behave best; `tweet65`/`hfs65` are moderate risk under heavier TCP payloads; `wget65` is highest risk because it depends on precise W5100 shared-access RECV/TX pointer semantics that current emulation partially shortcuts.

**References:** `ip65/apps/Makefile`, `ip65/apps/date65.c`, `ip65/apps/tweet65.c`, `ip65/apps/hfs65.c`, `ip65/apps/wget65.c`, `ip65/apps/w5100.c`, `pico/uthernet2.c`, `pico/uthernet2_net.cpp`.

---

## 10h. `wget65`-priority follow-up fixes

**What:** Apply targeted firmware changes for `wget65` shared-access compatibility after the app walkthrough.

**Why / root cause:**

- `wget65` commits partial RX chunks through `w5100_receive_commit(rcv)` and expects unread bytes to remain in the socket ring. Forcing `RX_RD -> sn_rx_wr` on every RECV loses unread tail data.
- `wget65` can queue larger TCP payload slices than 2 KiB before a SEND command. A single-shot 2048-byte staging buffer truncates one SEND command.

**What we did:**

- In `pico/uthernet2.c`, SN_CR=RECV no longer rewrites RX_RD to `sn_rx_wr`; RECV now acknowledges without discarding unread data, matching W5100 host-managed RX_RD semantics.
- In `pico/uthernet2.c`, TCP SEND path now drains the full queued TX range in chunks (1 KiB loop) instead of truncating to a single 2 KiB buffer.
- Verified by rebuilding both firmware targets with `./pico/build-both.sh` (RP2040 + RP2350), success.

**Takeaway:** These two changes directly target `wget65`'s shared-access behavior and should substantially reduce body truncation/drop risk compared with the previous telnet-oriented RECV shortcut.

**References:** `pico/uthernet2.c` (`send_data`, `W5100_SN_CR_RECV`), `ip65/apps/w5100.c` (`w5100_receive_commit`, `w5100_send_commit`).

---

## 10i. TCP RX backpressure fix (ack only accepted bytes)

**What:** Fix the remaining `wget65` reliability risk where TCP input could be acknowledged to lwIP even when not accepted by emulated W5100 RX.

**Why:**  
Previously `u2_tcp_recv()` called `tcp_recved(tpcb, p->tot_len)` unconditionally after attempting RX push. If emulated RX ring was full, data could be dropped while still ACKed to peer.

**What we did:**

- Changed U2 RX callback contract (`u2_push_rx_fn`) to return **accepted payload bytes**.
- Updated `u2_push_rx()` in `uthernet2.c`:
  - UDP remains atomic (all-or-drop datagram).
  - TCP now allows partial enqueue up to current free RX space and returns accepted length.
- Updated `u2_tcp_recv()` in `uthernet2_net.cpp` to call `tcp_recved()` only for bytes actually accepted into emulated RX.
- Rebuilt both release targets with `./pico/build-both.sh`; no lint errors in edited files.

**Takeaway:**  
TCP flow control now reflects emulated RX capacity, preventing “acknowledged-but-lost” bytes under sustained receive pressure (`wget65`/`hfs65` bulk transfers).

**References:** `pico/uthernet2_net.h`, `pico/uthernet2.c`, `pico/uthernet2_net.cpp`.

---

## 11. Summary table of code locations

| Topic | Key files | Decision / fix |
|-------|-----------|----------------|
| U2 address range | `defines.h`, `busloop.c` | C0x4–C0x7 only; no GPIO slot select |
| ip65 DHCP / MACRAW MAC | §1j, `uthernet2_net.cpp` | **STA netif** (`CYW43_ITF_STA`) for TX/RX hook/SA; **SHAR** + Ethernet **SA** + DHCP **`chaddr`** + **UDP** csum; **`tcp_bind`**(**Sn_PORT**) before **CONNECT** |
| AppleWin vs Pico U2 | §1i, AppleWin `Uthernet2.cpp` `IO_C0` / `MemReadFloatingBus` | AppleWin: **synchronous** slot I/O in one emulated step. Pico: **FIFO + PIO prefetch + IRQ0** (§1d §1f). AppleWin port = **`addr & 0x03`** on full slot page; MegaFlash U2 path only **`addr` 4–7**; **`$C0C8+`** not mirrored (ACIA reservation, §1b) — PIO **chunk = A3:A2** |
| ip65 W5100 probe + SHAR | `uthernet2.c` §`u2_reset`, §1c | RTR $07/$D0 + RMSR/TMSR $06 for `w5100.s` probe; default SHAR = `00:08:DC:A2:A2:A2` (matches `w5100.s`) so RMSR=$06 short path does not leave `cfg_mac` all-zero |
| Two DATA reads both $07 | §1f, `uthernet2.c` `auto_increment` | Pointer only advances when **MR** has **AI** ($02); **$03** = IND+AI; **$00** after reset or **$01** → no increment, second read still RTR0 |
| First `$C0C7` wrong then RTR OK | §1f, `busloop.c`, `U2_PeekDataPort` | RP2350 PIO prefetches next read’s byte; **`r[7]`** must hold **`U2_PeekDataPort()`** after each U2 cycle so first DATA read after addr setup isn’t stale |
| **`async_context` PANIC at `ck=5`** | §1g, `main.c`, `u2_monitor.h` | **`U2_MonPollFlush`** only on **core 0** — not from **`U2_Poll`** on core 1 |
| MACRAW RX `drop (no room)` / DHCP stall | §1g, `uthernet2.c` `u2_socket_discard_rx` | On overflow, discard unread RX (RX_RD→wr) then accept new frame |
| UART vs “Device not found” | §1c, `debug/*.log` | `w5100.s` `init` only `SEC`s on RTR XOR; correct RTR reads ⇒ that run passed Ethernet init; **`ip65_init` then `clc`s unconditionally** — see §1c if UI still says device not found; **48× `DATA read`** after `mode=0x03` traces RMSR/SHAR/OPEN |
| UART boot identity | `main.c`, `build_id.h.in`, `CMakeLists.txt` | After reboot, scroll to **latest** `Megaflash DEBUG Firmware…` block: includes **`FIRMWAREVERSTR`** and **`Firmware build:`** (UTC + Unix s from `build-both.sh`/`cmakeall.sh`; **`unknown` / `0`** if configured without timestamp) |
| ip65 init bisect (Pico 2 W Debug) | `CMakeLists.txt` `U2_IP65_CHECKPOINT`, `uthernet2.c`, `u2_monitor.c` | Default **quiet**: one **`[u2] ck=n`** per run when **`U2_IP65_CHECKPOINT=n`** (1=MODE 0x03 … 5=MACRAW OPEN). Optional **`U2_IP65_TRACE_DATA`**, **`U2_MON_LOG_BUS`** (floods UART) |
| ADTPro crash triage (`system-$01`) | §1p, `debug/uart_log.txt` | If no `sock0 OPEN/SEND/RECV` or checkpoint lines appear in failure window, treat as pre-W5100 crash/handoff issue first; use `U2_IP65_CHECKPOINT=1` build to confirm first mode write is reached |
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
| Pico W USB path + IPC | `main.c` §7i | When **`!appleConnected`** at boot, **`core0Loop()`** is skipped; **`PicoW_ServiceCore0IpcAndNetwork(0)`** must still run so Test WiFi / TFTP FIFO + **`NetworkPump_PollOnce`** are serviced |
| Release USB vs Apple bus | `main.c`, `misc.c`, `a2bus.h` §7j | **`NDEBUG`**: bus emulation only when Apple **and** no USB host; USB terminal only when USB **and** no Apple; Debug builds exempt |
| CYW43 init + LED | `misc.c`, `udptask.cpp`, `network_pump.cpp` §7k | **`InitPicoLed`** calls **`cyw43_arch_init()`**; **`InitCyw43()`** / **`NetworkPump::Init()`** must not call **`cyw43_arch_init_*`** again — use **`cyw43_is_initialized`** and only **`cyw43_arch_enable_sta_mode()`** |
| Release testing | §7b, §7c | Test Release build (NTP/TFTP/WiFi) before shipping; use pre-release checklist (§7c) to avoid Debug-only validation |
| C0C4 diagnostic LED | §9 | **Removed**; was 1 s LED on `$C0C4` — use UART / LA / §2b |
| nDEVSEL sense | Both PIO files | Active-low (trigger on low); inverted sense was tried and reverted |
| TFTP/UDP performance | `udptask.h`, `udptask.cpp` | HEARTBEAT_PERIOD 50→10 ms; 50 ms added latency per packet; blocking flash erase also stalls loop (see §13) |
| TFTP OOM panic at start | §13d, `misc.c`, `network_pump.cpp`, `ramdisk.c` | Panic is **pico_malloc** (`new` for ~2.5 KiB); **CP “RAM disk off”** does not free **`ramdisk_data[]`**; **`DebugPrintHeapState("NETPUMP: TFTP pre-new")`** before TFTP `new` |
| TFTP OOM panic at start | §13b, `misc.c`, `network_pump.cpp`, `ramdisk.c` | Panic is **pico_malloc** (`new` for ~2.5 KiB); **CP “RAM disk off”** does not free **`ramdisk_data[]`**; **`DebugPrintHeapState("NETPUMP: TFTP pre-new")`** before TFTP `new` |
| TFTP hostname default | `busloop.c`, `cpanel/tftp.c` | After command that sets data buffer, push chunk 0 to PIO immediately (§7g); clear hostname if it looks like status text |
| TFTP upload block count UI | `tftptxtask.cpp`, `tftprxtask.cpp` | Set `tftp_state.tsize` (TX) / WiFi status (RX) in `EvtStart()`; pump path does not call `Run()` (§7h) |
| CP version + clock | `cpanel/asm-megaflash.s` | `CMD_GETFIRMWAREVER` → cols 20–31; `CMD_GETTIMESTR` → 32–39; `ClearTime` clears 20–39 (§10c) |
| Flash JEDEC at boot | `flash.c` `ChipIDToCapacity` | §16: capacity from type+capacity bytes only; manufacturer byte ignored |
| Flash validate (Applesoft + C soak) | `tools/flash-validate/` | §17-18 + §19 + §20 + §21 + §22: `FLASHVAL.BAS` baseline + `FLASHSOAK.BAS` overnight CSV/TFTP + `TFTPUTIL.BAS` (80-col startup, auto slot detect preferring 4, host FQDN/IP prompt + `TFTPUTIL.CFG` default persistence, volume list by unit number + name before selection); `TFTPUTIL.TXT` shipped to disk as `TFTPUTIL.DOC`; `FLASHSOAK/flashsoak.c` + `Makefile` (cc65 → `flashsoak.bin`); `build-flashval-disk.sh` → `FLASHVALID.po` |
| WGET65V on-screen ip65 trace | `tools/wget65-verbose/`, §1h | Fork of `wget65` + register dumps; **`eth_init`** fixed **slot 4** (`$C0C4`); optional **`WGET65V`** on **`FLASHVALID.po`** when **`wget65v.bin`** built locally |
| Drives Enable toggles | `cpanel/drivesenable.c` | `gotoxy` Y is WNDTOP-relative; do not add `YPOS` (§10d) |
| Git 1.1.x patches | branch `1.1.x` | `checkout 1.1.x` to patch/build; `checkout main` to resume tip (§10e) |
| NetworkPump entry | `network_pump.cpp`, `network.cpp`, `main.c` | `RunNTP` / `RunTestWifi` / `RunTFTP` register a short-lived `LegacyUdpSessionAdapter` and spin `PollOnce()` until `GetCompleted()`; `CUDPTask::Run()` still wraps `EnterRunSession` + same loop for any direct caller; Core 0 idle `NetworkPump_PollOnce` (§14.8) |
| lwIP DNS/UDP vs `runningObject` | `udptask.cpp`, `network_pump.{h,cpp}` | DNS: `dns_pending_owner_` (`INetworkSession*`) + `OnDnsGetHostByNameResult` (§14.11), with pending-owner armed before `dns_gethostbyname` to avoid fast-callback race/timeouts. UDP: `NetworkPump_LegacyUdpRecv` + pcb→`INetworkSession*` (`udp_pcb_owners_`); `OnUdpRecvPbuf(pcb,p,…)` → `NotifyUdpReceived` or U2 (§14.10, §14.10b) |
| Uthernet II lwIP | `uthernet2_net.{h,cpp}`, `uthernet2.c`, `main.c` | Pump/TX/RX remain in U2 net layer; `U2_Net_Poll` is **core-0-only** (guarded) and is called from `PicoW_ServiceCore0IpcAndNetwork`. MACRAW TX called from bus/core1 is queued and executed on core0 to avoid wrong-core `async_context` panic (§1k, §14.10b). A full-pointer TX arithmetic experiment in `send_data` regressed ip65 detection and was reverted; ring math remains masked/consistent with TX_FSR (`§1l`). |
| Telnet65 walkthrough + fixes | §10f, `uthernet2.c`, `uthernet2_net.cpp`, `network_pump.cpp` | Implemented: TX wrap fix (`<0`), UDP/TCP chained pbuf flattening (`pbuf_copy_partial`), removed duplicate lwIP lock in `U2_Net_OpenUdp`; later `wget65` follow-ups changed RECV to preserve unread bytes (§10h) |
| Other ip65 tools walkthrough | §10g, `ip65/apps/*`, `ip65/apps/w5100.c`, `uthernet2*.{c,cpp}` | Risks: RECV discards unread bytes (shared-access mismatch), TCP RX overflow can drop+ACK, SEND chunk currently capped at 2048; `wget65` highest risk |
| `wget65` priority fixes | §10h, `uthernet2.c`, `ip65/apps/w5100.c` | SN_CR=RECV no longer forces `RX_RD->WR`; TCP SEND drains entire queued TX data in chunks (not single 2 KiB cap) |
| TCP RX backpressure | §10i, `uthernet2_net.h`, `uthernet2.c`, `uthernet2_net.cpp` | U2 RX callback returns accepted bytes; TCP `tcp_recved()` now acknowledges only accepted payload (UDP unchanged/all-or-drop) |
| Open C0C4 unresolved item | §12 | Slot decode confirmed working; remaining investigation is nDEVSEL signal/timing visibility at Pico/PIO point vs timing/FIFO/CPU-drain behavior |
| Pump TCP + session timers | `network_pump.{h,cpp}` | `CreateTcpPcb`: `tcp_arg(owner)`, `NetworkPump_LegacyTcpRecv` / `NetworkPump_LegacyTcpErr` → `OnTcpRecvPbuf` / `OnTcpErr`; `tcp_pcb_owners_` for unregister. `ScheduleTimer` / `CancelTimer`; `PollOnce` → `DrainSessionTimers` → `OnTimer` (§14.12) |

---

## 12. Open / unresolved: C0C4 not seen by firmware

**Observed:** With a logic analyzer, A0–A4 (and thus the address) are confirmed at the Pico when C0C4 is accessed. With nDEVSEL pull-up enabled, the Pico still does not recognize the access (no U2 response).

**Update:** Slot decode is confirmed working correctly for this path.

**Implication:** The PIO only runs `in pins` + `push` when **nDEVSEL (GPIO 20) goes low**. Remaining possibilities are:

1. **nDEVSEL signaling/visibility issue on this path** (line not observed low at the PIO at the critical point, or electrical/timing behavior differs from expected), or
2. **nDEVSEL goes low but something else** prevents the PIO from pushing or the CPU from reading (e.g. timing, FIFO, wrong SM on RP2040).

**Next steps for debugging:**

- **Probe nDEVSEL (GPIO 20)** during a C0C4 access at the Pico pins/PIO timing point. If it is not seen low at the right instant, treat as a signal/timing path issue; if it is seen low, focus on timing/FIFO/CPU-drain behavior.
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

**What we did:** Added `tools/flash-validate/FLASHVAL.BAS` and `README.md` (file format `FLASHVAL1`, slot base formula, volatile fields). **`build-flashval-disk.sh`** builds a standard **143360-byte ProDOS 140K** **`FLASHVALID.po`** using the same mechanism as **`../a2speed/Makefile`** (`-pro140`, then **`-p` / `-bas` / `-ptx`**). **PRODOS** and **BASIC.SYSTEM** are copied with **`-g`** from **`cpanel/prodos19.dsk`** (known-good image in-tree), not from padding **`pico/romdisk.po`** to 800K (that produced non-standard images some tools reject). **`TFTPUTIL`** (**`TFTPUTIL.BAS`**) is a standalone Applesoft TFTP upload/download tool ( **`CMD_TFTPRUN`** / **`CMD_TFTPSTATUS`**, same parameter layout as **`FLASHSOAK`**) so the validation disk offers TFTP without shipping the Control Panel **`BIN`**. **`Makefile`** in **`tools/flash-validate/`** mirrors **`make disk`** entry points (Homebrew **`java`**, default **`AppleCommander-ac.jar`**). **`-bas`** adds tokenized **FLASHVAL** from **`FLASHVAL.DSK.BAS`** (screen-only suite), **`-ptx`** adds **`FLASHVAL.SRC`**. Port variables must not be named **`PR`**: Applesoft treats **`PR`** as **`PRINT`**, and AppleCommander’s bastools fails with **`Expecting: [PR, #]`**; use **`PX`** (param port) and **`D1`** (data port) instead.

**What we didn’t do:** Destructive tests (`CMD_FORMATDISK`, `CMD_ERASEDISK`, `CMD_WRITEBLOCK`); those need explicit write-enable key handling and should stay a separate tool. Full **`FLASHVAL.BAS`** is not reliably **`-bas`**-tokenized (file I/O and tokenizer quirks); disk boot program is **`FLASHVAL.DSK.BAS`**.

**References:** `tools/flash-validate/README.md`, `tools/flash-validate/build-flashval-disk.sh`, `tools/flash-validate/Makefile`, `tools/flash-validate/FLASHVAL.BAS`, `tools/flash-validate/FLASHVAL.DSK.BAS`, `tools/flash-validate/TFTPUTIL.BAS`, `cpanel/prodos19.dsk`, `common/defines.h`, `pico/cmdhandler.c`.

---

## 18. Overnight volume/TFTP soak validator (`tools/flash-validate/FLASHSOAK.BAS`)

**What:** Added a second Applesoft validator, **`FLASHSOAK.BAS`**, designed for unattended overnight cycles with CSV logging and destructive media verification. It exercises each writable MegaFlash unit (including RAM disk when enabled) with a repeatable sequence: format, file workload (create/append/delete/fill), whole-volume checksum, TFTP upload to `192.168.0.10` as `validationX.po`, reformat, TFTP download, checksum compare, and cycle-level pass/fail logging.

**Why:** `FLASHVAL` validates command-path integrity and selected reads, but not sustained write/format/file/TFTP churn. The soak tool targets long-duration reliability and data-integrity regressions across flash + RAM media and network image round trips.

**What we did:** Implemented command wrappers in Applesoft for `CMD_GETDEVSTATUS` (`0x11`) **unit count**, `CMD_GETUNITSTATUS` (`0x12`) **block count**, `CMD_FORMATDISK` (`0x1D`), `CMD_READBLOCK` (`0x15`), `CMD_TFTPRUN` (`0x50`), and `CMD_TFTPSTATUS` (`0x51`) with firmware-compatible parameter ordering (including write-enable key `0x71` and null-terminated hostname/filename in data buffer). **Bug fix:** an early **`GOSUB 1500`** with **`U=0`** treated **`BC`** (block count / error garbage) as **unit count**, so **`FOR U = 1 TO UC`** could run to invalid units (e.g. **illegal quantity** on **`POKE`** / **`MID$`**). **Cycle start** now uses **`CMD_GETDEVSTATUS`** (`QC=17`) into **`UC`**, capped (**`16`**). Event logging uses a **ProDOS text (SEQ) file** — default **`SOAK.TXT`** — with **comma-separated** lines (columns **`ts`**, `cycle`, …); **`.CSV`** was avoided as the default extension because some ProDOS/BASIC paths handle **`.TXT`** more reliably with **`OPEN`** **`,T`**. **Append-only** (no **`DELETE`**): **`ts`** from ProDOS **`TIME$`** (8 chars). **`GOSUB 9000`** (**`RUN_START`**) runs at the **start of cycle 1** (not right after the server prompt), so **`OPEN`** happens only after the **PROGRESS** line; line **21** prints **`OPENING …`** immediately before ProDOS **`OPEN`**; **`Q1`/`NONE`** skips all log file I/O (**Applesoft** names significant to **two characters**, so **`LGSKIP`**/**`LGINIT`** both alias **`LG`** — use **`Q1`**/**`Q2`**). **`OPEN`**/**`WRITE`** match **`FLASHVAL.BAS`** (filename concatenated directly after **`OPEN`**/**`WRITE`**). **`OPEN`** **`T,A`** falls back to **`T,W`** + header if the file is missing. The disk build adds tokenized **`FLASHSOAK`** and **`FLASHSOAK.SRC`** to the same **140K** **`FLASHVALID.po`** as §17 (see **`build-flashval-disk.sh`** / **`Makefile`**). **Applesoft:** ProDOS **`PRINT D$`** lines must not spell **`OPEN`/`WRITE`/`CLOSE`/`DELETE`/`PREFIX`** as single literals (syntax error); use the **`"O"+"PEN"`** style as **`FLASHVAL.BAS`** (§17).

**What we didn’t do:** This pass does not include an external host-side parser/aggregator; the output is intentionally plain comma-separated text for downstream tooling. We also did not attempt to make this non-destructive.

**On-screen progress:** `FLASHSOAK` updates **VTAB 21** (40 columns) via **`GOSUB 8600`** after major steps; **`LEFT$(M$+B$,40)`** pads with spaces so shorter messages do not leave stale text (important when **`NONE`** disables the log and **`OPENING`** never runs).

**References:** `tools/flash-validate/FLASHSOAK.BAS`, `tools/flash-validate/README.md`, `tools/flash-validate/build-flashval-disk.sh`, `pico/cmdhandler.c` (`DoGetDeviceStatus`, `DoGetUnitStatus`, `DoFormatDisk`, `DoReadBlock`, `DoTFTPRun`, `DoTFTPStatus`), `common/defines.h`.

---

## 19. FLASHSOAK C port (`tools/flash-validate/FLASHSOAK/`)

**What:** A cc65 **apple2enh** implementation of the same soak sequence as **`FLASHSOAK.BAS`**: interactive slot / log path / TFTP host; **`CMD_GETDEVSTATUS`** / **`CMD_GETUNITSTATUS`**; format, ProDOS file workload, whole-volume 16-bit checksum, TFTP up/down, second checksum compare, cycle **`CYCLE_END`** then per-unit **reformat**.

**Why:** C is easier to extend than Applesoft for long-running loops (checksum over every block) and keeps the same register semantics as the BASIC **`49280+16*SL`** base instead of hard-coding **`$C0C0`** (see `cc65/megaflash.c`).

**What we did:** **`flashsoak.c`** uses slot-relative **`$C080 + slot×16`** pointers; **`mf_read_block`** matches BASIC **`MS`/`RE`** handling; **`mf_format_disk`** reads **`RE`/`ME`** like **`PEEK`** after **`CMD_FORMATDISK`**; **`checksum_volume`** propagates read errors via **`rd_err`**; **`REFORMAT`** logs **`RE`/`ME`** (not a bogus **`mf_issue_cmd(0)`**); **`status_line21`** avoids a ternary mixing **`const char*`** with **`""`** (cc65 **incompatible pointer types** on that line — use **`if (!msg) m = ""`**); **`file_workload`** returns failure if **`FILL.TXT`** cannot be created. **`Makefile`** builds **`flashsoak.bin`** with **`cl65 -t apple2enh -C ../../../cpanel/apple2enh-bin.cfg`** (same load address as cpanel **`$0A00`**).

**What we didn’t do:** **`build-flashval-disk.sh`** is not yet updated to **`BIN`**-import **`flashsoak.bin`** (optional follow-up); paths like **`/VAL1/A.TXT`** assume ProDOS volume names match **`PREFIX`** / mount points like the BASIC disk.

**References:** `tools/flash-validate/FLASHSOAK/flashsoak.c`, `tools/flash-validate/FLASHSOAK/Makefile`, `cpanel/apple2enh-bin.cfg`.

---

## 20. `TFTPUTIL.BAS` startup/UX updates (80 columns, auto-slot, host default)

**What:** Updated the standalone Applesoft TFTP helper (`tools/flash-validate/TFTPUTIL.BAS`) so it starts in 80-column mode, auto-detects MegaFlash slot placement (prefer slot 4), and prompts for the TFTP host as **FQDN or IP** with a persisted default.

**Why:** The prior flow required manual slot entry every run and an ambiguous host prompt. This slowed repeated testing and made operator mistakes more likely when the card was moved from slot 4 or when host naming was unclear.

**What we did:** Added `PR#3` at startup for 80-column display, replaced manual slot input with a probe routine that first checks slot 4 then scans slots 1-7, and reports whether it found MegaFlash in slot 4 or elsewhere. The host prompt now explicitly says `TFTP HOST FQDN OR IP` and shows the current default in-line. Added lightweight config load/save routines using `TFTPUTIL.CFG` (single-line text hostname/IP) so the chosen host becomes next-run default.

**What we didn’t do:** We did not add strict hostname/IP validation in Applesoft (keeping compatibility and code size simple), and we did not change TFTP command semantics (`CMD_TFTPRUN`/`CMD_TFTPSTATUS`) or transfer timeout behavior.

**Takeaway:** The utility now aligns with recurring operator workflow: direct launch in 80-column mode, no slot question, slot-4 preference with fallback discovery, and clearer/persistent TFTP host input.

**References:** `tools/flash-validate/TFTPUTIL.BAS`, `tools/flash-validate/README.md`, `pico/cmdhandler.c`, `common/defines.h`.

---

## 21. Flash validation disk: include `TFTPUTIL` docs as on-disk text

**What:** Added a dedicated text document for the Applesoft TFTP helper and included it in the generated `FLASHVALID.po`.

**Why:** `TFTPUTIL` behavior has grown (80-column startup, slot auto-detect, persisted host default), so users need an on-disk quick reference without opening repo files on a host machine.

**What we did:** Added `tools/flash-validate/TFTPUTIL.TXT` and updated `tools/flash-validate/build-flashval-disk.sh` to import it as `TFTPUTIL.DOC` using AppleCommander `-ptx`. Updated `tools/flash-validate/README.md` disk contents list to document the new file.

**What we didn’t do:** No tokenized BASIC changes in this step; this is documentation packaging only.

**Takeaway:** Every new `FLASHVALID.po` now carries `TFTPUTIL` usage notes directly on disk as `TFTPUTIL.DOC` (when `TFTPUTIL.TXT` exists).

**References:** `tools/flash-validate/TFTPUTIL.TXT`, `tools/flash-validate/build-flashval-disk.sh`, `tools/flash-validate/README.md`.

---

## 22. `TFTPUTIL.BAS`: list available volumes before unit selection

**What:** Enhanced `TFTPUTIL.BAS` to show a unit picker list (`unit - volume name`) before the user selects the local MegaFlash unit for TFTP upload/download.

**Why:** Numeric-only unit entry is error-prone when multiple volumes are present. Showing names at selection time improves usability and reduces accidental writes to the wrong unit.

**What we did:** Added `CMD_GETDEVSTATUS` (`0x11`) query to get unit count (`UC`) and print `AVAILABLE VOLUMES:` list for units `1..UC` (capped at 16). For each unit, issued `CMD_GETVOLINFO` (`0x14`) and parsed the ProDOS-style name-length nibble from byte 0 (`len = b0 & 0x0F`) to extract/display the volume name; fallback is `(UNKNOWN)` when parsing fails or command returns error. Unit input now enforces `1..UC`.

**What we didn’t do:** No deep validation of volume metadata beyond printable-name extraction; non-standard names/metadata remain shown as `(UNKNOWN)`.

**Takeaway:** Unit selection now presents human-readable volume names, making transfers safer and faster in multi-volume setups.

**Later revert:** A menu option to run ProDOS `CATALOG` on the selected volume from the upload/download prompt was added then removed: TFTP has no remote directory listing, and the local-only catalog was not worth the extra menu complexity.

**References:** `tools/flash-validate/TFTPUTIL.BAS`, `tools/flash-validate/TFTPUTIL.TXT`, `common/defines.h`, `pico/cmdhandler.c`.

*This document reflects reasoning and changes made during development; it may be extended as further design decisions are documented.*

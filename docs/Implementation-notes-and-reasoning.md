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

**What:** Structured tracing of Uthernet II emulation for serial capture (UART **460800** in Debug `main.c` after `stdio_uart_init`).

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

**Why (no room):** **`u2_netif_input_wrapper`** feeds **every** incoming Ethernet frame into socket 0 **MACRAW** while WiFi is up; the emulated **4K** RX ring can fill faster than the II drains it.

**Overflow policy (W5100 parity):** If the next complete MACRAW record (**2+len**) does not fit, **drop that frame** only — **no** internal flush of unread data. See **§1au** (replaces the earlier **`u2_socket_discard_rx()`** “wipe ring and retry” helper used for DHCP convenience).

**References:** `pico/uthernet2.c` (`u2_push_rx_macraw`), `pico/main.c`, `pico/u2_monitor.h`.

---

## 1au. MACRAW RX buffer full — match W5100 (reject new frame only)

**Requirement:** Emulated Uthernet II RX behavior when an Ethernet frame arrives but the socket RX memory cannot store the **complete** next MACRAW record (**2-byte length** + **Ethernet payload**).

**Why:** On real **W5100**, if there is **not enough free RX space** for the incoming packet, that packet is **not received** — the chip does **not** discard older unread bytes by moving **RX_RD** to free space. Older MegaFlash code called **`u2_socket_discard_rx()`** (set **RX_RD** to **sn_rx_wr**) when space was tight so DHCP could “make room”; that was **not** datasheet behavior and could hide host-side flow-control bugs.

**What we did:** **`u2_push_rx_macraw`** returns without writing if **`used + (2+len) > size`**, or if **`2+len > size`**. Removed **`u2_socket_discard_rx`** from this path.

**What we did not do:** No staging FIFO outside the chip (that would be an emulation extension, not W5100-accurate RX RAM).

**Project policy (re-stated 2026-05):** The emulated **RX** window stays **W5100-sized** per **RMSR** / ip65 — **one** tight budget like the real chip. If a new inbound frame does not fit as a **whole** **2+len** record, it is **dropped** (§**1au**). We **do not** grow the fake RX RAM, add side staging that changes **in-order** semantics, or advance **RX_RD** / discard head data to “make room” — older MegaFlash experiments did that and traffic was lost **out of sequence**, which broke predictable behaviour. **Order of work:** (1) **consistent, reliable** (even slow) throughput with strict W5100 rules; (2) only later, optional **throughput** improvements that do not regress ordering.

**References:** `pico/uthernet2.c` (`u2_push_rx_macraw`, `get_rx_rsr`).

---

## 1az. Inbound parity implementation (2026-05): drop-new MACRAW, atomic RX_WR publish, deterministic RMSR mapping

**Requirement:** Keep MegaFlash queue architecture unchanged, but align inbound RX buffer behavior with real W5100 semantics for MACRAW/UDP/TCP.

**Why:** Unusual inbound failures were more consistent with buffer-policy mismatches than with missing throughput. Two parity gaps mattered most: (1) MACRAW overflow still had a discard-old path in code, and (2) cross-core producer (`sn_rx_wr`) vs consumer (`Sn_RX_RD`) reads relied on plain loads/stores during burst traffic.

**What we changed (without queue redesign):**

- **`pico/uthernet2.c`**
  - Added explicit W5100 parity comments near `get_rx_rsr()` and `Sn_CR=RECV` contract.
  - Removed the MACRAW discard-old helper path; `u2_push_rx_macraw()` now **drops incoming** frame when `free < (2+len)` and never mutates `Sn_RX_RD` to make room.
  - Added release/acquire publication helpers for `sn_rx_wr` and switched enqueue paths to use a local write cursor with one publish at the end.
  - Consolidated `RMSR/TMSR` decode into `u2_apply_socket_sizes()` so size mapping is deterministic; when RX map over-allocates and must clamp, emit telemetry.
- **`pico/u2_monitor.h` + `pico/u2_monitor.c`**
  - Added `U2_MonNetRxDrop(...)` with protocol/reason fields (`no-room`, `partial`, `frame-too-big`, `size-map-clamped`) to make inbound pressure visible without changing queue mechanics.

**What we did not do:**

- No changes to `NetworkPump`, deferred queue topology, core split, or `busloop.c` timing pipeline.
- No expansion of effective RX capacity beyond W5100-sized windows.

**Validation:** Built with `MF_DEBUG_BUILD_NO_GIT_COMMIT=1 ./build-debug.sh` (success, `pico2_debug/megaflash.uf2`).

**Takeaway:** Inbound buffer semantics now match the real-chip model more closely: preserve unread head data, drop only incoming units when full, and expose pressure conditions explicitly.

**Follow-up (TX appeared broken after parity merge):** Resetting **`sn_rx_wr` to zero on every host **`RMSR`** write desynchronized RX bookkeeping vs **`Sn_RX_RD`** and could stall TCP/ACK-driven flows (egress looked dead). **Fix:** remap **`sn_rx_wr`** with **`old_wr % new_receive_size`** instead of zeroing. **`get_tx_data_size` / `Sn_TX_FSR`** now guard **`transmit_size == 0`** so TMSR clamp does not compute **`mask = size - 1`** garbage. Optional **`U2_MACRAW_COMPAT_DROP_OLDEST=1`** (CMake / **`build-debug-both.sh`**) restores one-shot unread discard before rejecting a MACRAW frame when full (compat, not strict W5100). **`U2_MonNetRxDrop`** is rate-limited per socket/reason (**100 ms**) to reduce debug UART contention under floods.

---

## 1aw. ADTProETH “host timeout” — May 2026 UART + `tcpdump` (rx_noroom)

**Capture (May 2026 session):** `debug/2026-05-04 11-14-47 FT232R USB UART #1.log` (tail for last boot). **Wire:** use a **dated `tcpdump.pcap`** paired by wall-clock + **`Firmware build:`** — **do not** treat repo **`tcpdump.txt`** (or any stale text export) as “current” without matching timestamps (see `debug/README.md`).

**UART (firmware with `[u2eth]` / `[u2tap]` / `[u2macraw]`):**

- **`[u2macraw] tx_q_drop=0`**, **`tx_q=0`** — deferred MACRAW TX ring is not the bottleneck.
- **`rx_noroom=2`** (non-zero on the **~5 s** stats line) — **`u2_push_rx_macraw`** rejected **two** inbound frames (§**1au**) because **`used + (2+len) > receive_size`** while the II had not **RECV**’d enough yet.
- Immediately before, **`[u2eth] RX`** logged **many** identical **~342 B** frames (DHCP **OFFER**-shaped **`67→68`** inside Ethernet) and **`[u2m] net sock0 MACRAW rx len=342`** in a tight burst — the AP/router can flood offers; the emulated **S0 RX** budget (from **RMSR** / ip65) is finite, so **drops** are plausible even when ADTPro later “works” at the Ethernet edge.
- Opening garbage on line 1 of the capture (`x<xxx<…`) matches **serial terminal baud ≠ firmware** (that run was still **115200** on the Pico; terminal mismatch garbles until the next clean boot block).

**`tcpdump`:** Shows **many** Mac → Pico **large** UDP/6502 replies and **short** Pico → Mac polls (`UDP length 9`). **`bad udp cksum`** on inbound Mac packets is a **known capture/offload artifact** on some macOS paths — do not treat as MegaFlash corruption unless validated on the wire without checksum offload.

**Interpretation (aligned with §1au policy):** **`rx_noroom=2`** means **two** frames were **correctly rejected** — same class of event a real **W5100** would produce when the **II has not RECV’d** enough to free space and the wire keeps delivering (e.g. DHCP **OFFER** bursts). That is **not** a signal to **enlarge** the emulated RX buffer in firmware: the project explicitly keeps **W5100-sized** RX and **drops** when full until **reliable in-order** behaviour is proven end-to-end (**§1au** “Project policy”). **ADTProETH / ip65** timeouts in that situation point to **application / network timing** (drain **RECV** fast enough, or fewer duplicate offers on the LAN), **not** a missing MegaFlash “bigger ring” hack.

**Optional diagnostics (do not change RX capacity):** Less UART **`printf`** load (e.g. **`U2_ETH_HEADER_TRACE=0`**, **460800** already set) reduces core0 time stolen from **`U2_Net_Poll`** — helps **service** ingress but does **not** relax W5100 RX rules.

**References:** `debug/2026-05-04 11-14-47 FT232R USB UART #1.log`, `pico/uthernet2_net.cpp` (`u2_netif_input_wrapper`), `pico/uthernet2.c` (`u2_push_rx_macraw`), `pico/main.c` (UART baud), `debug/README.md` (pcap + UART line semantics).

---

## 1av. STA `netif->input` hook — install from core 0 after CYW43 is up

**Symptom:** ip65 / MACRAW **TX** could work (DHCP DISCOVER leaves the Pico) but **RX** never filled **`u2_memory`** — **`[u2eth] RX`** appeared on UART while **`[u2m] net sock0 MACRAW rx`** did not after OPEN.

**Why:** **`U2_Init()`** runs before **`InitPicoLed()`** / **`cyw43_arch_init()`**. **`SN_CR OPEN`** (MACRAW) runs on **core 1**. **`U2_Net_OpenMacraw`** only hooked **`sta->input`** when **`cyw43_is_initialized()`** was already true — otherwise **`u2_netif_input_wrapper`** was **never** installed and **`push_rx_macraw_cb`** never ran.

**What we did:** **`u2_macraw_sta_input_hook_ensure()`** in **`U2_Net_Poll()`** (core 0, **`cyw43_arch_lwip_begin/end`**) attaches the wrapper once socket 0 is **MACRAW**, CYW43 is up, and **`sta->input`** exists. **`OpenMacraw`** only updates **SHAR** when possible.

**References:** `pico/uthernet2_net.cpp` (`u2_macraw_sta_input_hook_ensure`, `U2_Net_Poll`).

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
  - `U2_Net_SendMacraw()` enqueues when called off-core (see §**1ag**: **ring queue**, not a single slot — rapid SEND must not overwrite an unsent frame).
  - `U2_Net_Poll()` (core 0) drains queued MACRAW frames and performs `linkoutput` there.
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

## 1r. Core0 lwIP poll cadence, deferred MACRAW TX queue, TCP SEND batching

**Requirement / symptom:** Improve effective Uthernet throughput and robustness when core 1 issues network work while lwIP/CYW43 runs on core 0 — specifically: (a) **`PicoW_ServiceCore0IpcAndNetwork`** previously **`multicore_fifo_pop_timeout_us`’d first** then **`U2_Net_Poll`**, so up to tens of milliseconds could pass **without** **`NetworkPump_PollOnce`** when **`core0Loop`** used a **50 ms** FIFO timeout; (b) **MACRAW TX** deferred from core 1 used **one** pending buffer → overlapping sends could overwrite; (c) TCP **`send_data`** issued **`U2_Net_SendTcp`** per chunk with **`tcp_output`** each time.

**Why / design:**
- **Poll before block:** Serving **`U2_Net_Poll`** immediately on each core-0 entry bounds idle latency regardless of FIFO behavior; shortening the **`core0Loop`** wait from **50 ms** to **1 ms** caps worst-case **sleep-in-FIFO** delay while NTP waits (still bounded by **`time_reached(nextUpdateTime)`**).
- **MACRAW ring:** A small queue avoids overwrite under bursty TX from the bus; **RP2040** stays **RAM-tight** — ring depth **3** vs **8** on **RP2350**. When the queue had been empty and core 1 enqueues the first frame, a **non-blocking** **`multicore_fifo_push_timeout_us`** with **`IPCCMD_NET_WAKE`** lets core 0 exit a blocking pop sooner if the FIFO was the only waiter (harmless duplicate wakes).
- **TCP:** **`U2_Net_SendTcpEnqueue`** + **`U2_Net_SendTcpFlush`** keep **`tcp_write`** per chunk but **one `tcp_output`** per logical SEND. Chunk size stays **1024 bytes** on the stack in **`send_data`** — raising toward **MSS (1460)** increased the **core 1** stack frame and **linked with heap/stack overlap** on **Pico W (RP2040)** in practice.

**What we did:** Reordered **`main.c`**; added **`IPCCMD_NET_WAKE`** in **`ipc.h`**; ring + wake in **`uthernet2_net.cpp`**; **`uthernet2_net.h`** / **`uthernet2.c`** enqueue+flush API and **`send_data`** TCP loop.

**What we didn’t do:** Did not raise **`TCP_MSS`** / **`TCP_SND_BUF`** in **`lwipopts.h`** in this pass; did not use **1460-byte** stack buffers on RP2040 until RAM/stack layout allows.

**References:** `pico/main.c`, `pico/ipc.h`, `pico/uthernet2_net.{h,cpp}`, `pico/uthernet2.c`.

---

## 1s. Pico 2 W-only throughput follow-up: fix TCP TX data loss under lwIP backpressure

**Symptom after prior tuning:** UDP reliability regressed and TCP showed recovery behavior but still slow transfer plus corrupted payloads.

**Root cause identified:** In `send_data()` TCP path, we were batching chunks and then always moving `Sn_TX_RD` to `TX_WR` even when `tcp_write()` could not accept all queued bytes (`ERR_MEM` path). That acknowledges data to the emulated W5100 that lwIP never queued, producing silent byte loss/corruption and retransmit-heavy behavior.

**What we changed (Pico 2 W target):**

- `U2_Net_SendTcpEnqueue()` now returns accepted byte count (`0` on reject) instead of `void`.
- `send_data()` advances `Sn_TX_RD` by **accepted bytes only**, and stops enqueueing when a partial/zero accept is seen.
- `U2_Net_SendTcpFlush()` is called once only if at least one chunk was accepted.
- Chunk size set to **1460** for Pico 2 W (`U2_TCP_SEND_CHUNK`), matching `TCP_MSS`, to reduce per-SEND overhead.
- Deferred MACRAW queue depth increased to **16** and the extra wake-message FIFO push removed to reduce unnecessary IPC churn while validating reliability.

**What we did not change in this pass:** `PicoW_ServiceCore0IpcAndNetwork` poll-before-wait behavior and 1 ms core0 FIFO timeout remain, since those changes improve lwIP service cadence and are not in the corrupt-data path.

**Takeaway:** For W5100 SEND emulation, host-visible pointer movement must reflect **actual lwIP acceptance**, not requested bytes. Throughput tuning (chunking/flush frequency) is safe only when this accounting is strict.

**References:** `pico/uthernet2.c`, `pico/uthernet2_net.h`, `pico/uthernet2_net.cpp`.

---

## 1bb. Re-land strict TCP TX accounting + UDP no-truncation on current API (2026-07)

**What/why:** A data-flow review of the send path (mostly MACRAW in use, but TCP/UDP paths still exercised) flagged two defects that had regressed on the current simplified API. The strict TCP accounting from §1s/§10j had been reverted along with §10j–§10p (see §10l re-land note), so at HEAD `send_data()` again advanced `Sn_TX_RD` all the way to `Sn_TX_WR` regardless of lwIP acceptance, and the UDP path capped a datagram at a 2 KiB stack buffer while still draining the full window.

**Root cause:**
- **Item 2 (TCP):** `U2_Net_SendTcp()` returned `void` and always issued `tcp_write`/`tcp_output`; `send_data()` unconditionally set `Sn_TX_RD = Sn_TX_WR`. On `ERR_MEM`/`tcp_sndbuf` exhaustion the un-queued bytes were acknowledged to the emulated W5100 but never handed to lwIP → silent hole in the TCP stream (out-of-sequence / truncated transfer as the peer sees it).
- **Item 3 (UDP):** `send_data()` copied into `uint8_t buf[2048]`, clamped `n` to 2048, then advanced `Sn_TX_RD` past the **whole** datagram → any datagram >2 KiB was silently tail-truncated.

**What we did:**
- `U2_Net_SendTcp()` now returns `int` (accepted bytes, `-1` on socket error), clamping the write to `tcp_sndbuf(pcb)`; on `ERR_MEM` it returns `0`. Header/stub updated to match.
- `send_data()` tracks a `consumed` counter and advances `Sn_TX_RD` by `rd_full + consumed` (only accepted bytes). TCP loop stops on partial/zero/fatal accept, leaving the remainder in the **FIFO TX ring**; because the ring is FIFO and a compliant W5100 driver checks `Sn_TX_FSR` before writing, the remainder flushes **in order** on the host's next SEND (no loss, no reorder). Backpressure still returns `0` from `send_data()` so `Sn_CR` clears normally.
- **Item 3:** `U2_Net_SendUdp()` signature changed to take ring params (`ring_base, ring_size, start_off, len, …`) and copies straight from the (wrapping) TX ring into the `PBUF_RAM` pbuf. This removes the 2 KiB truncation **and** needs no large intermediate buffer.

**Dead end (reverted):** First cut of item 3 used a `static uint8_t udp_buf[8192]` (max single-socket TX region) to avoid stack pressure. That added 8 KiB to `.bss` and overflowed **RP2040 Debug** RAM by 8652 bytes at link. Replaced with the ring→pbuf copy (RAM-neutral). Chosen because `MEM_LIBC_MALLOC=1` in poll mode means pbufs come from the heap and the TX ring is power-of-two sized, so masked wrap-copy into the pbuf is clean.

**Build status:** `MF_DEBUG_BUILD_NO_GIT_COMMIT=1 ./pico/build-debug-both.sh`. `pico2_debug` (RP2350 Debug) and `pico_release` (RP2040 Release) link OK. **RP2040 `pico_debug` fails to link with a pre-existing 460-byte RAM overflow** — confirmed identical at HEAD via `git stash`, so it is unrelated to this change (open issue: RP2040 Debug image is over-budget on RAM).

**References:** `pico/uthernet2.c` (`send_data`), `pico/uthernet2_net.{h,cpp}` (`U2_Net_SendTcp`, `U2_Net_SendUdp`), `pico/lwipopts.h` (`TCP_SND_BUF`, `MEM_LIBC_MALLOC`).

---

## 1cc. ip65 "Device not found" DEFINITIVELY reproduced: RTR probe vs RP2350 reactive-FIFO throughput (2026-07-11)

**What (symptom):** Stock cc65/ip65 (contiki + telnet65) reports **"Device not found"** on a physical Apple **IIc** with the RP2350 image, even though the UART trace shows the emulator returning the **correct** RTR bytes (`$0017→0x07`, `$0018→0xD0`). This is the long-standing §1c/§1d/§1f "correct RTR yet device-not-found" contradiction.

**The actual detection code (authoritative):** Fetched `drivers/w5100.s` from cc65/ip65. `init` does, after `sta mode` (MR=0x03 → we log `mode=0x03`) and `set_addr($0017)`:

```asm
 lda #$07^$D0     ; A=$D7
 eor data         ; A ^= read($0017)  -> RTR0, auto-inc to $0018
 eor data         ; A ^= read($0018)  -> RTR1
 beq :+           ; A==0 -> present; else sec/rts = "Device not found"
```

Two **back-to-back** `$C0C7` reads. It does **no** data-port writes before this (only MODE/ADDRHI/ADDRLO ports), so the debug DATA-**write** trace added earlier was a dead end. If it passes, the very next access is a read of **RMSR `$001A`** — absent from every failing trace, so the driver takes the `sec/rts` failure branch.

**Reproduced at bus speed (Apple IIc monitor, machine code):** Two consecutive `LDA/LDX $C0C7` after `set_addr($0017)` returned **`08 08`** (not `07 D0`); the literal `EOR` check returned **`$D7`** (≠0) → detection would fail. `0x08` is **RCR at `$0019`** — a stale leftover (the pointer is momentarily `$0019` right after the addr-hi write, before the addr-lo write). Keyboard-paced monitor reads of the same registers return the correct `07, D0, 08`. So the failure appears **only** for machine-speed back-to-back reads. (Aside: my first test used the driver's slot-0 *template* addresses `$C084–$C087` by mistake — on a IIc those are the language-card soft switches, returning floating-bus garbage `7D`/`A0 A0`. Correct card I/O for this setup is `$C0C4–$C0C7`.)

**Root cause:** On RP2350 the `a2bus` SM (SM1) fulfills a `$C0C7` read **entirely from the prefetched `rxf_putget` FIFO** (`mov osr, rxfifo[y]`); it never asks core 1 during the cycle. Core 1 (`busloop.c`) must therefore refresh that FIFO with the **auto-incremented next byte** *between* consecutive reads — a window of only ~one 6502 cycle (~585 RP2350 cycles at 150 MHz for reads 4 cycles apart). In the **Debug** build, the per-access monitor work done *before* the FIFO refresh — `U2_MonBus` (`U2_MON_LOG_BUS`), the `read_value()` checkpoints, and `U2_MonDataReadTrace` — starves that window, so SM1 re-presents a **stale** byte. The firmware trace still logs correct values because `read_value()` computes them correctly; only the byte latched **on the bus** is stale. This is exactly the failure the existing `a2bus_rp2350.pio` comment anticipated ("ip65 RTR XOR fails on-bus while a later C dump looks correct"), and the same class as §1d/§1f — but here proven to be a **throughput/Heisenbug**, not a store-visibility bug. Note ip65's data-movement (`mov_data`, `poll`) also uses tight consecutive `$C0C7` accesses, so this must be fast enough for ip65 to work **at all**, not just to pass detection.

**What we did (this step):** Built fresh **Release** firmware (`./build-both.sh`, ts 1783795258 = 2026-07-11 18:40:58 UTC) → `pico2_release/megaflash.uf2`. In Release, `UTHERNET2_DEBUG=0` removes all per-access monitor work from `read_value()`/`U2_HandleBusAccess`, so the busloop should keep the FIFO current. Asked the user to flash it and retry telnet65/contiki as the decisive test.

**Open / next:**
- If Release connects → confirmed debug-overhead Heisenbug. Then trim the Debug hot path: refresh the SM1 FIFO **first** (compute read byte + `U2_PeekDataPort` next byte, `UpdateMegaFlashRegisters(1,…)`), and defer `U2_MonBus`/trace pushes to *after* the FIFO update so tracing no longer perturbs bus timing.
- If Release still fails at -O2 → the reactive-FIFO scheme is structurally too tight; fix so SM1 does not depend on a per-read core-1 refresh (e.g. compute/prefetch the auto-increment "next byte" without the full monitor path, or move DATA-port auto-increment closer to the PIO).

**References:** cc65/ip65 `drivers/w5100.s` (init RTR probe); `pico/busloop.c` (U2 branch, `registers.r[7]` prime + IRQ0 wait + `UpdateMegaFlashRegisters(1,…)`), `pico/a2bus_rp2350.pio` (`mov osr, rxfifo[y]`), `pico/a2bus.h` (`UpdateMegaFlashRegisters`), `pico/uthernet2.c` (`read_value`, `U2_HandleBusAccess`, `U2_MON_LOG_BUS`); §1c §1d §1f §1w.

**Outcome:** Confirmed — user flashed the release image and **card detection now works** (telnet65 sustained a ~10 min stream once). The debug per-read `critical_section` (`u2_mon_push`) armed by `U2_IP65_TRACE_DATA` exactly at MR=0x03 was the timing culprit; a debug build for ip65 must set `U2_IP65_TRACE_DATA=0` (net-layer socket/MACRAW traces don't touch the bus read path and are safe to keep).

---

## 1cd. Intermittent ip65 DNS = ARP sender-hardware-address mismatch + unfiltered MACRAW RX (2026-07-11)

**What (symptom):** With detection fixed (§1cc), telnet65/Contiki sessions are **intermittent** — one ~10 min session worked, but usually DNS fails. `debug/megaflash-2.pcap` contains only **4 outbound ARP requests** (`who-has 192.168.0.1 tell 192.168.0.234`, Eth src = STA MAC `88:a2:9e:48:22:7a`) and **no replies**: ip65 is stuck in ARP retry for the gateway/DNS server, so DNS never starts. (Tooling note: this session, files under `~/Documents` intermittently returned 0 bytes to non-`tcpdump` processes — macOS TCC/lock quirk; `tcpdump -r` decoded fine.)

**Root cause 1 — ARP SHA mismatch (recurrence of §1aa):** `u2_send_macraw_core0()` rewrote only the **Ethernet** source (`memcpy(eth+6, netif->hwaddr, 6)`) and DHCP `chaddr`; the **ARP payload sender-hardware-address** was left untouched. ip65's own MAC is the WIZnet OUI `00:08:DC:A2:A2:A2` (`w5100.s` `mac`), so every ARP carried SHA=`00:08:DC:..` while the Ethernet SA was the STA MAC. The gateway records `192.168.0.234 → 00:08:DC:..` and unicasts the ARP reply (L2) to that MAC, which the AP never delivers to our associated station (`88:a2:9e:..`) → reply lost. Intermittent because success depends on the gateway's ARP-cache state / whether it happens to reply to the frame source vs the ARP SHA. The §1ad "ARP SHA normalization" had been lost in a later rollback (§1z).

**Root cause 2 — MACRAW MAC filter ignored:** ip65 opens socket 0 with `Sn_MR=0x44` (MACRAW **+ MF**, from `w5100.s` `lda #$44`). A real W5100 with MF delivers only frames to its own MAC (SHAR) or broadcast. We ignored MF and copied **every** frame the STA netif sees (broadcast + multicast + our unicast) into the small RX ring via `u2_netif_input_wrapper` → `u2_push_rx_macraw`. On a busy LAN this floods the ring, so a late unicast ARP reply / DNS response hits "no room" and is dropped while the 1 MHz host drains slowly.

**What we did:**
- **Fix 1:** In `u2_send_macraw_core0()` (`pico/uthernet2_net.cpp`), after the Ethernet SA rewrite, normalize the ARP SHA too: `if (len>=28 && eth[12]==0x08 && eth[13]==0x06) memcpy(eth+22, netif->hwaddr, 6);` (ethertype 0x0806; ARP SHA at frame offset 22 = payload offset 8). Now the gateway learns our IP against the STA MAC and replies to a MAC the AP delivers.
- **Fix 2:** Added `W5100_SN_MR_MF 0x40` (`pico/w5100_regs.h`) and a MAC filter at the top of `u2_push_rx_macraw()` (`pico/uthernet2.c`): when the socket's `Sn_MR` MF bit is set, accept only dest MAC == `SHAR` (`u2_memory[W5100_SHAR0..]`) or broadcast; otherwise drop **silently** (normal hardware behavior, no monitor spam). SHAR is aligned to the STA MAC in `U2_Net_OpenMacraw`, so our own unicast (ARP reply, DNS, TCP/UDP) is accepted; ambient multicast/other-unicast is dropped, matching real U2 and freeing ring space.

**Why these are correct (not app workarounds):** Both restore W5100/real-Uthernet-II behavior the app assumes — a card whose L2 identity is consistent (SHA==SA) and that honors the MF bit it was configured with. Per the U2-emulation rule, this is closing an emulation gap, not patching ip65.

**Didn't do / open:** Did not pin `SHAR` to the STA MAC at write time (would remove all TX rewrites for ARP/DHCP/eth-SA in one place, but is more invasive and cross-layer; kept the minimal proven rewrite approach). If DNS is still intermittent after this, build a diagnostic debug with `U2_IP65_TRACE_DATA=0` (detection stays working) and watch `[u2m]` `MACRAW rx` / `rxdrop NO_ROOM` / socket lines to see whether the ARP reply now arrives and whether the ring still overflows; then consider cyw43 POLL cadence (§1r) or `U2_MACRAW_COMPAT_DROP_OLDEST=1`.

**Build:** `./build-both.sh` (ts ~2026-07-11 19:41 UTC) → `pico2_release/megaflash.uf2` + `pico_release/megaflash.uf2`, both link OK.

**References:** `debug/megaflash-2.pcap`; `pico/uthernet2_net.cpp` (`u2_send_macraw_core0`, `u2_netif_input_wrapper`, `U2_Net_OpenMacraw`), `pico/uthernet2.c` (`u2_push_rx_macraw`), `pico/w5100_regs.h` (`W5100_SN_MR_MF`, `W5100_SHAR0`); cc65/ip65 `drivers/w5100.s` (`mac`, `Sn_MR=$44`); §1aa §1ad §1j §1cc.

---

## 1ce. "Socket receive noise" = RX ring pointers not reset on OPEN (Contiki DNS/connect fail) (2026-07-12)

**What (symptom):** With DNS fixed (§1cd), Contiki "completely fails to pass the DNS query" and telnet "tries/fails to open a connection," and the socket receive is described as **very noisy**. UART from `pico2_debug` (build ts 2026-07-12 00:34) shows the story: NTP succeeds; a first `sock0 OPEN mr=0x44` (telnet, at 39056186 µs) drives a normal `SEND / MACRAW tx / MACRAW rx / RECV` cadence. Then a **second** `sock0 OPEN mr=0x44` (Contiki, at 135799129 µs) is immediately followed by an **unbounded `sock0 RECV` storm** — thousands of `RECV`, ~8 ms apart, sustained ~38 s — with only a handful of real inbound `MACRAW rx len=60` and, crucially, **no `MACRAW rx no_room`** anywhere.

**What it rules out:** No `no_room` during the storm ⇒ the RX ring is **not** overflowing (hypothesis A / §1cd Fix 2 pressure is not the failure here). The storm is the host side spinning, not inbound flood.

**Root cause — OPEN never reset the emulated RX ring, so `Sn_RX_RSR` never returned to 0.** ip65's `poll()` (cc65 `w5100.s`) loops: if `Sn_CR==0` and `Sn_RX_RSR!=0`, read a MACRAW frame (2-byte length prefix + payload), advance `Sn_RX_RD`, issue `RECV`; else report "no data." A real W5100 **initializes the socket's RX/TX ring pointers on the OPEN command**, so a freshly opened socket has `RSR=0`. Our emulation never did this. The first OPEN after a chip reset was fine only because `sn_rx_wr` and the host `Sn_RX_RD` happened to be 0. When Contiki opened MACRAW socket 0 **after** the telnet session, `sn_rx_wr` was still at telnet's leftover offset, so `Sn_RX_RSR = wr − rd ≠ 0`. ip65 then "receives" a garbage frame, advances `Sn_RX_RD` by a **bogus 2-byte length header** read from stale ring bytes, and `RSR = wr − rd` never converges to 0 (it wraps) → an infinite RECV loop → the "noise," and DNS/connect never make progress. (The first-vs-second-OPEN asymmetry in the log is the tell.)

**What we did:** Added `u2_reset_socket_rings(int i)` in `pico/uthernet2.c` that zeroes the producer (`sn_rx_wr` via `u2_rx_wr_store`) and the host-facing pointer registers (`Sn_RX_RD0/1`, `Sn_TX_RD0/1`, `Sn_TX_WR0/1` in `u2_memory`) so the socket presents `RSR=0` / full `FSR`. Called it at the **top of the `W5100_SN_CR_OPEN` handler** (before the protocol switch) so it runs for UDP/TCP/MACRAW alike — matching hardware, which resets pointers on OPEN regardless of protocol. ip65 re-reads `Sn_TX_WR/RD` after OPEN, so zeroing TX is safe (and correct).

**Why this is correct (not an app workaround):** A real W5100 gives a freshly OPENed socket an empty RX buffer; the emulation must too. This closes an emulation gap (stale ring across socket reuse), per the U2-emulation rule.

**History / why the reset is back on OPEN:** §10p had a `u2_socket_reset_rx` that zeroed `sn_rx_wr` + `Sn_RX_RD` on OPEN; §10q then moved it to **CONNECT/CLOSE only**, fearing an "OPEN-time RX_RD reset raced Contiki init ordering"; §10r ultimately `git restore`d the whole §10j–§10p bundle during the detection-bringup crisis, deleting the reset entirely. That crisis was later proven to be the **§1cc RTR Heisenbug** (debug per-read `critical_section` timing), *not* the ring reset. The UART storm here is direct evidence that OPEN **must** reset the ring, and MACRAW OPEN (no CONNECT) means CONNECT-only reset would never fire — so the reset belongs on OPEN. The old "race" concern doesn't apply to ip65: `w5100.s` OPEN writes `Sn_MR`/`Sn_PORT`, issues `Sn_CR=OPEN`, waits `Sn_CR==0`, and does not pre-seed `Sn_RX_RD`.

**Didn't do / open:** This fixes the Contiki RECV storm specifically. The **inbound-only TCP loss** from the §1cd wire-capture analysis (server→Apple retransmits: ~6.5% telnet, worse for HTTP/wget) is a separate issue — leading suspects (B) cyw43 RX poll cadence and (C) sustained tight-`$C0C7`-read corruption (RP2350 FIFO refresh race, §1f class) on long data-read loops. Re-evaluate after this build once the socket isn't spinning on phantom RSR.

**Build:** `make -C pico2_debug` (build-debug-both.sh still fails at the pre-existing RP2040 456-byte overflow) → `pico2_debug/megaflash.uf2`, ts 1783821111 (2026-07-12 01:51 UTC).

**References:** `pico/uthernet2.c` (`u2_reset_socket_rings`, `write_socket_register` OPEN case, `u2_rx_used_bytes`, `read_rx_rd_coherent`, `u2_push_rx_macraw`); `pico/uthernet2_net.cpp` (`U2_Net_OpenMacraw`, `U2_Net_RecvConfirm`); `pico/w5100_regs.h` (`W5100_SN_RX_RD0/1`, `W5100_SN_TX_RD0/1`, `W5100_SN_TX_WR0/1`); cc65/ip65 `drivers/w5100.s` (`poll`); §1cc §1cd §1f.

---

## 1t. Add temporary UART counters for transfer-error triage

**Requirement:** During Pico 2 W validation, expose lightweight runtime counters to confirm whether observed bad data / retries correlate with TCP enqueue backpressure (`tcp_write` reject) and/or deferred MACRAW queue pressure.

**What we did:** Added periodic (`~1s`) Debug UART line in `U2_Net_Poll()`:

- Format: `[u2ctr] tcp calls=<n> acceptB=<n> rej=<n> | macq enq=<n> drain=<n> drop=<n> peak=<n> q=<n>`
- `tcp calls`: number of `U2_Net_SendTcpEnqueue` attempts.
- `acceptB`: total bytes accepted by lwIP (`tcp_write == ERR_OK`).
- `rej`: enqueue attempts rejected (typically `ERR_MEM`).
- `macq enq/drain/drop`: deferred MACRAW frames queued, drained, and dropped (queue full).
- `macq peak/q`: high-water mark and current queue depth.

**Why:** This gives immediate visibility into where reliability degrades under load without adding heavy tracing. If `rej` climbs with low `acceptB` growth, TCP path is pressure-limited. If `drop` climbs, deferred MACRAW queue is the bottleneck.

**What we didn’t do:** No persistent telemetry storage or host-side parser yet; counters are cumulative from `U2_Net_Init()` and intended as temporary on-device diagnostics.

**References:** `pico/uthernet2_net.cpp` (`U2_Net_SendTcpEnqueue`, `U2_Net_SendMacraw`, `U2_Net_Poll`).

---

## 1u. Periodic U2 heap telemetry on UART (Pico 2 W)

**Requirement:** Report available RAM while U2 is active, roughly every 10-15 seconds, to detect low-memory pressure during real transfers.

**What we changed:** Extended the existing `[u2ctr]` periodic line in `U2_Net_Poll()` to include heap metrics from `GetFreeHeap()` / `GetTotalHeap()` and moved the report cadence to **10 seconds**.

- Added fields: `heap free`, `used`, `total`, and `min_free` (low-water mark since `U2_Net_Init`).
- Current line shape:
  - `[u2ctr] heap free=<n> used=<n> total=<n> min_free=<n> | tcp ... | macq ...`

**Why this location:** `U2_Net_Poll()` is the core-0 U2 network heartbeat, so heap snapshots are taken in the same execution context that drives lwIP and deferred MACRAW TX.

**Takeaway:** `min_free` provides the key safety margin under load; if it trends too close to zero during problematic transfers, remaining reliability issues are likely memory-pressure related.

**References:** `pico/uthernet2_net.cpp`, `pico/misc.{h,c}` (`GetFreeHeap`, `GetTotalHeap`).

---

## 1v. Follow-up: avoid per-poll heap sampling overhead

**Symptom:** After adding heap telemetry, telnet65/ip65 behavior regressed and Uthernet detection/traffic became unstable.

**Why:** The first version sampled `GetFreeHeap()` on **every** `U2_Net_Poll()` pass to track a true low-water mark. `GetFreeHeap()` uses allocator state (`mallinfo`) and is too heavy at poll-loop cadence, reducing time available for normal network pump work.

**Fix:** Sample heap only at the existing periodic report interval (10s), and update `min_free` from those periodic samples. This preserves RAM visibility with much lower runtime overhead.

**Takeaway:** Telemetry in hot network loops must be O(1)/cheap per pass; allocator introspection should be periodic, not per-iteration.

**References:** `pico/uthernet2_net.cpp` (`U2_Net_Poll` reporting block).

---

## 1w. ip65 “device not found” triage: reduce debug-path perturbation

**Symptom:** After instrumentation additions, telnet65/ip65 again reported “device not found.”

**Rationale:** In Debug, `U2_IP65_TRACE_DATA=1` emits many per-read logs from the U2 path after MR=0x03. This can perturb timing enough to confound diagnosis of true slot/probe issues.

**Action taken:** Rebuilt `pico2_debug` with minimal diagnostic noise and early checkpoint only:

- `U2_IP65_CHECKPOINT=1`
- `U2_IP65_TRACE_DATA=0`
- `U2_MON_LOG_BUS=0`
- `U2_ETH_HEADER_TRACE=0`

This keeps visibility of whether ip65 reaches the first mode write while avoiding DATA-read log flood.

**References:** `pico/build-debug-both.sh` options, `pico2_debug/CMakeCache.txt`.

---

## 1x. ADTProETH slowness/errors: prevent silent UDP/MACRAW TX drops

**Symptom:** ADTProETH sometimes starts but transfers are very slow and often fail before completion; Contiki web browsing remains comparatively stable.

**Why this can happen:** `send_data()` always advanced `Sn_TX_RD` to `TX_WR` at the end of SEND, but UDP/MACRAW send paths did not return whether lwIP queueing actually succeeded (e.g., `pbuf_alloc` failure, `udp_sendto` error, deferred MACRAW queue full). On those failures, the frame was effectively discarded while pointers still advanced, causing silent loss and retry-heavy behavior.

**What we changed:**

- `U2_Net_SendUdp(...)` now returns success/failure (`1/0`).
- `U2_Net_SendMacraw(...)` now returns success/failure (`1/0`), including queue-full rejection.
- `send_data()` now exits early (without pointer advance) when UDP/MACRAW TX is not accepted; pointers advance only for accepted sends.

**Takeaway:** W5100 TX pointer progression must be coupled to successful enqueue into the host networking stack. Decoupling them can look like “timing issues” from the client side because retries/recovery hide silent drops.

**References:** `pico/uthernet2.c` (`send_data`), `pico/uthernet2_net.h`, `pico/uthernet2_net.cpp`.

---

## 1y. Follow-up rollback: strict UDP/MACRAW acceptance harmed liveness

**Observed after §1x:** ADTPro host-query phase timed out and Contiki responsiveness dropped noticeably.

**Why likely:** The strict “don’t advance TX pointer unless UDP/MACRAW send accepted” policy was too aggressive for current software expectations and transient lwIP conditions, causing repeated/resubmitted command behavior that looked like hard stalls.

**What we changed:**

- Restored permissive UDP/MACRAW SEND behavior (no success-return gating for pointer progression).
- Kept TCP accepted-byte accounting from §1s (still needed to avoid TCP data corruption).
- Improved `U2_Net_Poll` fairness: run `NetworkPump_PollOnce()` **before** deferred MACRAW drain, and cap MACRAW drain work to **2 frames per poll** so burst MACRAW traffic cannot monopolize core-0 service time.

**Takeaway:** For this emulation, TCP needs strict acceptance accounting; UDP/MACRAW currently need more permissive/liveness-first behavior to preserve compatibility with client timing assumptions.

**References:** `pico/uthernet2.c`, `pico/uthernet2_net.{h,cpp}`.

---

## 1z. Rollback to known-good U2 runtime + enable `[u2eth]` packet-header tracing

**User direction:** ADTProETH remained unstable and Contiki responsiveness worsened; request was to roll back recent runtime edits and use packet-header tracing instead of periodic counter polling.

**What we did:**

- Restored U2 runtime files to repository baseline (`main`) state:
  - `pico/ipc.h`
  - `pico/main.c`
  - `pico/uthernet2.c`
  - `pico/uthernet2_net.cpp`
  - `pico/uthernet2_net.h`
- Built `pico2_debug` with packet-header tracing enabled:
  - `U2_ETH_HEADER_TRACE=1`
  - `U2_IP65_TRACE_DATA=0`
  - `U2_MON_LOG_BUS=0`
  - `U2_IP65_CHECKPOINT=0`

**Why:** This removes experimental poll/counter overhead and behavioral changes from the datapath, while preserving a low-intrusion network visibility signal (`[u2eth]` RX/TX header bytes) to diagnose ADTProETH timing/packet behavior.

**References:** `pico/uthernet2_net.cpp` (`U2_ETH_HEADER_TRACE` wrappers), `pico/build-debug-both.sh` flags.

---

## 1aa. ADTProETH timeout root cause from host tcpdump: ARP SHA mismatch

**Status:** Reverted in code at user request after confirming the immediate ADTProETH failure was host-side configuration (fixed IP vs DHCP), not firmware.

**Symptom:** ADTProETH host query timed out even though Pico could ARP for host `192.168.0.154`.

**Evidence:** Host-side tcpdump showed requests from IP `192.168.0.245` with:

- Ethernet source MAC = CYW43 STA (`88:a2:...`)
- ARP sender hardware address (SHA) = emulated W5100 default (`00:08:dc:a2:a2:a2`)

Host ARP reply then targeted `00:08:dc:a2:a2:a2` instead of STA MAC, so L2/L3 path broke before UDP exchange.

**Fix applied:** In `u2_send_macraw_core0()`:

- keep Ethernet SA rewrite to STA MAC,
- add ARP patch helper to rewrite ARP sender hardware address to STA MAC,
- re-enable DHCP BOOTP `chaddr` / option 61 patch call so sender identity stays consistent across ARP + DHCP.

**Takeaway:** For MACRAW on Wi-Fi STA, all sender identity fields must agree (Ethernet SA, ARP SHA, DHCP client MAC fields) or peers can cache/reply to an unreachable L2 address.

**References:** `pico/uthernet2_net.cpp`, `debug/tcpdump.txt`.

---

## 1ab. Correlated tcpdump+UART: MACRAW frames must not be forwarded to lwIP

**Observation:** During ADTPro UDP/6502 attempts, host tcpdump showed valid request/response datagrams, while UART simultaneously showed Pico transmitting ICMP type 3/code 3 (port unreachable) quoting those same UDP packets.

**Root cause:** `u2_netif_input_wrapper()` was feeding ingress frames to U2 MACRAW **and** forwarding the same pbuf into lwIP input. lwIP had no listener for ADTPro/U2 traffic and emitted ICMP unreachable, disrupting transfer flow.

**Original symptom / observation:** Duplicate delivery into lwIP could produce **ICMP dest/port unreachable** for UDP/TCP ports owned only by ip65/MACRAW (lwIP had no pcb).

**Baseline behavior (restored 2026-05, matches tree `483e8da`):** **`u2_netif_input_wrapper`** copies eligible ingress to **`push_rx_macraw_cb`**, then **always** **`return u2_saved_netif_input(p, inp)`** — **no** port filtering, **no** MACRAW-only `pbuf_free`. That keeps **MegaFlash** lwIP services (NTP, TFTP, ARP, DNS, …) on the **same STA netif** working whenever MACRAW is open.

**Tradeoff:** Unwanted ICMP from lwIP may recur when ip65’s ports are not lwIP-bound — triage by tcpdump if ADTPro misbehaves; do **not** guess ports/protocols for MACRAW traffic.

**References:** `pico/uthernet2_net.cpp` (`u2_netif_input_wrapper`), `debug/tcpdump.txt`, `debug/2026-04-28 15-03-58 FT232R USB UART.log`.

---

## 1ar. MACRAW + lwIP (2026-05): restore duplicate netif feed (`483e8da` semantics)

**What:** Removed §1ab-style “MACRAW owns ingress only” and removed §1ar port-based filtering. **`u2_netif_input_wrapper`** again matches **`git show 483e8da:pico/uthernet2_net.cpp`**: feed **U2 MACRAW** copy, then **always** chain **`u2_saved_netif_input`**.

**Why:** MegaFlash traffic handling **before** U2 RX ring work used this pattern; isolating lwIP broke Pico-side Wi‑Fi; guessing ports is invalid for arbitrary MACRAW payloads.

**References:** `pico/uthernet2_net.cpp`; §1ab observation text above.

---

## 1as. STA `netif->input` hook: defer to core 0 after CYW43 is up

**Symptom:** **`U2_Init()`** runs **before** **`InitPicoLed()`** / **`cyw43_arch_init()`**. **`SN_CR OPEN`** (MACRAW) runs on **core 1** from the Apple bus. The previous **`U2_Net_OpenMacraw`** path installed **`u2_netif_input_wrapper`** only when **`cyw43_is_initialized()`** was already true — if OPEN happened first, the hook was **skipped permanently**, so **duplicate STA ingress** (MACRAW tap **+** **`u2_saved_netif_input`**) never activated and the MegaFlash + ip65 combined path failed.

**What we did:** **`u2_macraw_sta_input_hook_ensure()`** runs from **`U2_Net_Poll()`** on **core 0** (inside **`cyw43_arch_lwip_begin/end`**) once socket 0 is **MACRAW**, CYW43 is up, and **`sta->input`** exists. **`OpenMacraw`** only updates **SHAR** when possible; it **does not** mutate **`netif->input`** from the bus core.

**References:** `pico/uthernet2_net.cpp` (`u2_macraw_sta_input_hook_ensure`, `U2_Net_Poll`).

---

## 1ac. New timeout attempts: no ICMP conflict now, but ARP SHA mismatch still present

**Observation:** In the newest `debug/tcpdump.txt`, ADTPro UDP/6502 traffic is bidirectional (host sends 632-byte datagrams; Pico replies with short 8-11 byte datagrams), and there are no ICMP port-unreachable packets in this capture window.

**Key failure indicator:** At `15:43:36`, Pico emits:

- Ethernet SA = `88:a2:9e:48:22:7a` (STA MAC),
- ARP request `tell 192.168.0.245`,
- Host ARP reply is sent to `00:08:dc:a2:a2:a2` (W5100 MAC from ARP payload SHA), not to `88:a2:...`.

This means L2 and ARP payload identity diverge again, so peers can cache/target the wrong MAC and ADTPro traffic becomes intermittent or times out.

**Why this matters now:** The MACRAW/lwIP double-handling issue appears mitigated (no ICMP unreachables here), so the remaining dominant failure in this trace is ARP sender identity mismatch.

**References:** `debug/tcpdump.txt` lines around `15:43:36`; prior ARP-SHA patch/revert in `pico/uthernet2_net.cpp`.

---

## 1ad. Minimal retry patch: ARP SHA normalization only (keep DHCP payload untouched)

**Requirement:** After broad MACRAW ingress filtering removed ICMP conflicts, keep changes minimal and retry ADTProETH timeout mitigation focused only on ARP identity consistency.

**What we changed:** In `u2_send_macraw_core0()`, after forcing Ethernet source MAC to STA MAC, we now also patch ARP payload sender hardware address (SHA) to the same STA MAC via `u2_macraw_patch_arp_sender_hwaddr()`.

**What we deliberately did not change:** We did not re-enable BOOTP/DHCP `chaddr` / option 61 rewriting for this test pass.

**Why:** Latest captures showed peers replying to `00:08:dc:*` due to ARP payload SHA mismatch while Ethernet SA was `88:a2:*`. This targeted patch aligns L2 and ARP identity without touching DHCP payload behavior.

**Build:** `pico/pico2_debug` rebuilt successfully after patch.

**References:** `pico/uthernet2_net.cpp` (`u2_send_macraw_core0`, `u2_macraw_patch_arp_sender_hwaddr`), `debug/tcpdump.txt` around `15:43:36`.

---

## 1ae. Retest outcome after ARP-SHA patch: faster startup, later mid-transfer TX stall

**Observation (user retest):** ADTProETH now connects and starts transfer much faster than before, but transfer still aborts.

**Correlated traffic clues from `debug/tcpdump.txt`:**

- ARP identity appears corrected in this run (`15:49:50` host ARP reply targets `88:a2:9e:48:22:7a`, not `00:08:dc:*`).
- Session on UDP/6502 begins normally:
  - host probes/requests (8–11B payloads),
  - Pico responds with larger data frames (commonly 410–578B payloads, occasional smaller frames).
- Near failure window, Pico large responses thin out and then stop, while host continues periodic 9B UDP/6502 packets (`15:50:28.819`, `15:50:29.558` etc.).

**Interpretation:** This no longer looks like ARP-misdirection or lwIP ICMP conflict. Current dominant signature is a **mid-transfer send-path stall/starvation on Pico/U2 side** (socket TX ring/producer-consumer pacing/state transition), after initially healthy throughput.

**Data gap:** Current `debug/uart_log.txt` still contains earlier boot/mDNS traces and does not include this exact crash window, limiting socket-state correlation.

**References:** `debug/tcpdump.txt` lines around `15:49:56`–`15:50:29`, `debug/uart_log.txt` (missing matching window).

---

## 1af. Add low-overhead UDP TX counters to locate post-start transfer stall

**Requirement:** After ADTPro transfer became fast-but-crashy, add lightweight runtime telemetry (not heavy packet dumps) to identify whether the Pico UDP send path fails by lwIP error, memory exhaustion, or silent stop.

**What we changed (`pico/uthernet2_net.cpp`):**

- Added counters:
  - `u2_udp_tx_attempts`
  - `u2_udp_tx_ok`
  - `u2_udp_tx_err`
  - `u2_udp_tx_pbuf_fail`
  - `u2_udp_tx_bytes`
  - `u2_udp_tx_last_err`
- In `U2_Net_SendUdp()`:
  - count each send attempt,
  - capture `udp_sendto()` return code (`ERR_OK` vs error),
  - track pbuf allocation failures as `ERR_MEM`.
- In `U2_Net_Poll()`:
  - print one low-rate diagnostic line every 5 seconds:
    - `[u2udp] tx att=... ok=... err=... pbuf_fail=... bytes=... last_err=...`

**Why this instrumentation:** It distinguishes:

- send API failures (`err` rising),
- allocation pressure (`pbuf_fail` rising),
- healthy API sends with upper-layer stall (`ok` rises but transfer still stops).

This should isolate whether the remaining crash is transport enqueue failure or higher-level socket/state logic.

**Build:** `pico/pico2_debug` rebuilt successfully.

**References:** `pico/uthernet2_net.cpp` (`U2_Net_Init`, `U2_Net_SendUdp`, `U2_Net_Poll`).

---

## 1ag. Root-cause refinement from fresh UART: MACRAW dominates; single deferred TX slot can lose frames

**Observation from fresh log (`2026-04-28 16-06-53 FT232R USB UART.log`):**

- `[u2udp] tx att=0 ok=0 ...` remains zero throughout the failing transfer window.
- At the same time, there is sustained heavy `sock0 MACRAW rx len=...` and frequent `sock0 SEND` / `MACRAW tx len=...`.

**Implication:** ADTPro transfer here is entirely on the MACRAW path, not `U2_Net_SendUdp()`. Therefore the UDP counters are useful negative evidence but not the active bottleneck.

**Likely failure mode:** Prior implementation deferred core1 MACRAW TX using only one pending slot (`u2_macraw_tx_pending` + single buffer). Under burst load this allows overwrite/loss of unsent frame data, which can desynchronize ADTPro state after some blocks (observed progression from hang around block 24 to ~36).

**Fix applied (2026-05-02, firmware):**

- Replaced single-slot deferred MACRAW TX with ring queue in `uthernet2_net.cpp`:
  - `U2_MACRAW_TX_Q_DEPTH` **16**, head + count under `u2_macraw_tx_cs`,
  - `u2_macraw_tx_q_drop` increments on enqueue when full,
  - `U2_Net_Poll()` drains up to **`U2_MACRAW_TX_DRAIN_PER_POLL` (8)** frames per visit (then `NetworkPump_PollOnce()`).
- `U2_Net_Close` on a **MACRAW** socket clears the queue (stale deferred TX).

**Expected diagnostic behavior next run:**

- **Debug UART** (about every **5 s**): **`[u2macraw] tx_q=… tx_q_drop=… rx_noroom=… rx_toobig=…`** from **`U2_Net_Poll`** (`uthernet2_net.cpp` + **`U2_MacrawRxReject*`** in `uthernet2.c`). See `debug/README.md` (**tail** the log end first).
- If **`tx_q_drop`** rises, raise **`U2_MACRAW_TX_Q_DEPTH`** / **`U2_MACRAW_TX_DRAIN_PER_POLL`** or reduce core0 starvation another way (not `main.c` FIFO reorder — MegaFlash regression).
- If **`rx_noroom`** rises: **expected** under §**1au** when inbound exceeds free **W5100-sized** RX — **do not** enlarge emulated RX or discard in-order tail to make room (project policy). Correlate with **II RECV rate** / host bursts; **`rx_toobig`** = frame larger than socket **receive_size**.

**References:** `pico/uthernet2_net.cpp` (`U2_Net_SendMacraw`, `U2_Net_Poll`, MACRAW queue state).

---

## 1ax. MACRAW: couple `Sn_TX_RD` advance to accepted TX (queue or linkoutput)

**What:** After the core1 **MACRAW TX ring** (§**1ag**), **`send_data()`** still advanced **`Sn_TX_RD` → `Sn_TX_WR`** whenever **`U2_Net_SendMacraw`** returned — but **`void`** return and **`(void)`** on **`u2_macraw_tx_queue_enqueue`** meant **queue full** and **`pbuf_alloc`** failure **still advanced** the emulated W5100 TX pointers. The **II** then believed bytes had left the chip while nothing was queued or on the wire → “TX broke” / stuck retries / ADTPro timeouts.

**Why:** The ring queue fixed **overwrite** of deferred frames; it did not fix **W5100 register semantics** vs **acceptance**.

**What we did:** **`U2_Net_SendMacraw`** returns **`0`** if the frame is **accepted** (copied into the ring on core1, or **`u2_send_macraw_core0`** succeeded on core0), **`-1`** otherwise. **`send_data()`** for **MACRAW** returns **without** updating **`TX_RD`** on **`-1`**. **`U2_Net_Poll`** drain: if **`u2_send_macraw_core0`** fails after dequeue, **put the slot back at the queue head** and stop draining for that poll (retry next poll). **`u2_send_macraw_core0`** returns **`bool`**. UDP path unchanged (§**1y** liveness rollback still applies there).

**Follow-up (same §):** An initial version still cleared **`Sn_CR`** to **0** after **`send_data()`** returned **`-1`**, so ip65 believed **SEND** completed while **no** frame was accepted and **`Sn_TX_RD`** did not move — **outbound stopped** and **tcpdump** on the Mac saw nothing. **Fix:** on **`W5100_SN_CR_SEND`**, if **`send_data()`** **≠** **0**, **return** from **`write_socket_register`** without clearing **`Sn_CR`**; **`U2_TryCompletePendingSocket0Send()`** (core **0**, end of **`U2_Net_Poll`**) retries **`send_data(0)`** when socket **0** **`Sn_CR=SEND`** and **MACRAW**; core **1** **`U2_Net_SendMacraw`** also **spins** briefly on full **TX** queue so core **0** can drain.

**Debug UART (2026-05 follow-up):** **`[u2macraw]`** every **~5 s** now includes **`lo_err`** / **`not_rdy`** / **`pbuf_fail`** (failures inside **`u2_send_macraw_core0`**). **`U2_Net_Poll`** calls **`U2_TryCompletePendingSocket0Send`** **after** draining the deferred MACRAW queue **and** again **after** **`NetworkPump_PollOnce`**. Pair with a **non-empty** **`tcpdump.pcap`** on the **correct** Mac interface; an **empty** pcap means **no packets matched** the filter/interface, not “proof” the Pico UART lied — use **`MACRAW linkout`** lines + **`lo_err`** to separate **CYW43 refused TX** vs **capture path**.

**What we didn’t do:** Did not revert the **ring queue** or increase **`U2_Net_Poll`** drain cap as the primary fix — this is pointer correctness, not throughput.

**References:** `pico/uthernet2_net.h`, `pico/uthernet2_net.cpp` (`U2_Net_SendMacraw`, `U2_Net_Poll`, `u2_send_macraw_core0`), `pico/uthernet2.c` (`send_data`).

---

## 1ah. New regression clue: reset-abort path fires mid-transfer

**Observation:** In the fresh UART session for the timeout/stall attempts, log stream contains `NETPUMP: RequestAbortAll()` in the same transfer window where ADTPro flow collapses. This abort path tears down active session timers/sockets via `NetworkPump::RequestAbortAll()`.

**Context:** `main.c` currently calls `NetworkPump_RequestAbortAll()` directly from `nRESET` falling-edge GPIO IRQ callback. If `nRESET` chatters/noises, multiple rapid false triggers can abort networking during a valid transfer.

**What we tried first (insufficient):**

- Added simple IRQ debounce in `gpio_intr_callback`:
  - process only `GPIO_IRQ_EDGE_FALL`,
  - ignore edges occurring within 50ms of prior accepted edge.

**Why this was not enough:** A fresh transfer that stalled at block 27 still logged `NETPUMP: RequestAbortAll()` in-window. That means we still accepted at least one false edge long enough to trip abort, so "time since previous edge" alone is not robust enough.

**Mitigation update (current):**

- `gpio_intr_callback` no longer calls abort functions directly.
- IRQ now only latches "reset fell" timestamp/pending flag.
- Core 0 service path (`PicoW_ServiceCore0IpcAndNetwork`) runs `ServiceAppleResetAbort()` which:
  - confirms `nRESET_PIN` is still low,
  - requires low hold for 250ms before aborting,
  - rate-limits repeat aborts (500ms minimum gap).
- Only then it calls `NetworkPump_RequestAbortAll()` and `AbortEraseFlashDisk()`.

**Rationale:** This converts a fragile edge-triggered abort into a level-confirmed abort, which is much harder to trigger from line noise/chatter while preserving intentional reset behavior.

**Follow-up from block-37 retest:** UART still showed `NETPUMP: RequestAbortAll()` in the transfer-stall window, so the 50ms confirm was still too permissive for this setup. Increased confirm/guard to 250ms/500ms to bias strongly against transient dips.

**Follow-up after 250ms/500ms change (two-run retest):**

- New ADTPro session starts later (`sock0 OPEN ...` around `16:29:38`) with no subsequent `NETPUMP: RequestAbortAll()` in that run window.
- Transfer still fails (one run returns to prompt, another stalls around block 16), but failure signature changed:
  - no reset-abort log,
  - no MACRAW queue pressure (`mq_drop=0`, `mq_cur=0` in periodic reports),
  - MACRAW RX/TX continues during stall window.

**Takeaway:** reset-chatter abort was a real contributor earlier, but current stall is now likely a protocol/state issue on the MACRAW data path rather than queue overflow or explicit network-pump abort.

**Build:** `pico/pico2_debug` rebuilt successfully after moving abort handling out of IRQ.

**References:** `pico/main.c` (`gpio_intr_callback`), `pico/network_pump.cpp` (`NetworkPump::RequestAbortAll()`).

---

## 1ai. ADTPro stall instrumentation: socket-0 pointer progression around SEND/RECV

**Requirement:** Capture where MACRAW progress stops after reset-abort noise was mitigated (`RequestAbortAll` no longer appears in failure windows).

**Why:** Recent captures show active MACRAW traffic with `mq_drop=0`, but transfers still stall. Existing monitor lines (`sock0 SEND/RECV`, `MACRAW rx/tx len`) do not show whether W5100 pointer progression (`TX_RD/TX_WR/RX_RD/RX_WR`) stalls, jumps, or diverges.

**What we changed:**

- Added a new monitor event `U2_MonSockPtrs(...)` in `pico/u2_monitor.{h,c}`.
- Event logs compact snapshots:
  - phase (`send-pre`, `send-post`, `recv-pre`, `recv-post`),
  - `TX_RD`, `TX_WR`, `RX_RD`, `RX_WR`,
  - socket status `SR`.
- Instrumented `pico/uthernet2.c` `write_socket_register()` for socket commands:
  - `SN_CR_SEND`: snapshot before and after `send_data(i)`.
  - `SN_CR_RECV`: snapshot before and after `U2_Net_RecvConfirm(i)`.

**Notes:**

- W5100 has no `SN_RX_WR` register; `RX_WR` in this diagnostic is the emulator’s internal `sn_rx_wr` for that socket.
- This is Debug-monitor-only visibility (compiled to stubs when activity monitor is disabled).

**Build:** Rebuilt `pico/pico2_debug` with `./build-debug.sh pico2` successfully after adding the new monitor event.

**Takeaway:** The next UART run should tell us whether stalls happen with pointers still advancing (protocol-level deadlock) or with one edge frozen (specific command/ack progression break).

**Follow-up from first run with this instrumentation (block ~37 crash, then immediate first-block stall):**

- Crash run still showed one `NETPUMP: RequestAbortAll()` event before the later socket-open window.
- In the immediate-stall run, socket 0 entered a long `RECV` loop where:
  - `TX_RD == TX_WR` (no outbound progression),
  - `RX_RD` repeatedly stayed fixed while MACRAW RX kept arriving.
- Pointer snapshots highlighted a representation mismatch risk: emulated `sn_rx_wr` was stored as **masked ring offset** while host-visible `RX_RD` is a **full 16-bit progressing pointer**.

**Fix applied (pointer-consistency hardening):**

- `sn_rx_wr` is now kept as a full 16-bit monotonic pointer.
- RX buffer indexing still masks (`sn_rx_wr & (size-1)`), but host-visible pointer operations preserve full progression.
- `u2_socket_discard_rx()` now writes full `sn_rx_wr` to `SN_RX_RD0/1` (not masked low ring offset).

**Why this matters:** with heavy MACRAW traffic and wraps, masked-only `sn_rx_wr` can drift from host full-pointer semantics and leave `RX_RSR`/RECV behavior inconsistent after wrap/discard cycles, matching the observed repeated-RECV stall pattern.

**Build:** `pico/pico2_debug` rebuilt successfully after this change.

**Follow-up from the latest block-19 stall (after UART 460800 + reduced RECV logging):**

- `sock0 ptrs recv-pre` still showed pathological `RX_RD` jumps while `RX_WR` progressed smoothly.
- During the stall window, inbound MACRAW frames continued (`rx len=60/63/...`) and queue drops stayed at zero, but pointer snapshots were noisy/non-physical.

**Root-cause hypothesis:**

- `RX_RD` is host-written as two 8-bit writes (`SN_RX_RD0` then `SN_RX_RD1`) on the bus side.
- Emulator reads of `RX_RD` (`get_rx_rsr()` and monitor snapshots) were plain 16-bit byte-pairs with no coherence guard.
- With cross-core overlap, this can produce torn `RX_RD` values (mixed old/new bytes), which explains random pointer jumps and can miscompute `RX_RSR` (false full/empty), leading to stalls.

**Fix applied (coherent RX_RD read):**

- Added `read_rx_rd_coherent()` in `pico/uthernet2.c` (high/low/high retry) to avoid torn 16-bit `RX_RD` reads.
- Switched both:
  - `get_rx_rsr()` to use coherent `RX_RD`,
  - `mon_sock_ptrs_snapshot()` to log coherent `RX_RD`.

**What we did not change:**

- No protocol behavior changes in `U2_Net_RecvConfirm()`.
- No additional queue depth or timing throttles in this patch.

**Takeaway:** this patch targets shared-pointer coherency rather than throughput knobs; if block stalls were triggered by torn `RX_RD` accounting, this should stabilize both `RX_RSR` math and pointer traces under ADTPro load.

**Build:** rebuilt `pico/pico2_debug` via `./build-debug.sh` successfully after the coherence patch.

**Follow-up after first coherence patch (remaining issue):**

- New traces still showed implausible `RX_RD` jumps, indicating the initial coherence read was still permissive for some byte-write interleavings.
- The initial method (`high/low/high`) only guarantees coherence when high-byte transitions are observed in one direction.

**Refinement:**

- Updated `read_rx_rd_coherent()` to read the full 16-bit register twice and require an exact match before accepting (`v1 == v2`).
- This is byte-order agnostic and rejects torn combinations for either write ordering.

**Build:** rebuilt `pico/pico2_debug` via `./build-debug.sh` successfully after this refinement.

**Follow-up after additional retest (connect/list/parse instability, no download start):**

- UART still showed non-physical `RX_RD` progressions (`recv-pre` cycling/teleporting values) even after coherence-read refinement.
- Failures shifted earlier in ADTPro flow (host timeouts / "garbage received"), suggesting RX accounting corruption affects control/list parsing before block transfer.

**Root cause update:**

- Coherent reads alone are insufficient when `SN_RX_RD0/1` are shared bytes modified on core1 while core0 RX producers advance `sn_rx_wr`.
- The emulated "source of truth" for consumed RX pointer should not depend on racing reads from raw register bytes.

**Fix applied (stable RX_RD shadow):**

- Added per-socket `sn_rx_rd` shadow pointer (`u2_socket_t`) in `pico/uthernet2.c`.
- On host writes to `SN_RX_RD0/1` (`write_socket_register`), update `sn_rx_rd` from the composed register pair.
- `get_rx_rsr()` now uses `sn_rx_rd` (stable shadow) instead of reading raw register bytes.
- `u2_socket_discard_rx()` now updates both `SN_RX_RD0/1` memory bytes and `sn_rx_rd`.
- `mon_sock_ptrs_snapshot()` now reports `sn_rx_rd`, so pointer diagnostics reflect the same value used by RX accounting.

**Takeaway:** RX capacity/consumption math now runs on a stable per-socket pointer model rather than race-prone byte reads, which should reduce early-session parser corruption and host timeout spirals.

**Build:** rebuilt `pico/pico2_debug` via `./build-debug.sh` successfully after the shadow-pointer patch.

**Follow-up from next retest (no parse timeouts, but transfer resets/stalls remain):**

- User-observed behavior improved in session setup/list parsing (no timeout spiral), but transfer attempts still failed mid-run (reset around block ~37, stall around block ~31).
- `recv-pre` snapshots still showed impossible `RX_RD` movement during hot loops because shadow updates were still occurring per-byte write to `SN_RX_RD0/1` (capturing transient half-written states).

**Final refinement (protocol commit semantics):**

- Removed per-byte `sn_rx_rd` updates on `SN_RX_RD0/1` register writes.
- Latch `sn_rx_rd` only at `SN_CR_RECV` handling, which is the W5100 host commit point after RX_RD programming.
- This prevents intermediate byte-write values from polluting `RX_RSR` accounting and pointer diagnostics.

**Build:** rebuilt `pico/pico2_debug` via `./build-debug.sh` successfully after RECV-latch refinement.

**References:** `pico/uthernet2.c` (`write_socket_register`, `send_data`), `pico/u2_monitor.h`, `pico/u2_monitor.c`.

---

## 1aj. Reset-abort isolation during Uthernet/ADTPro transfers

**Requirement:** Stop transfer-session resets caused by Apple reset IRQ handling when no legacy NetworkPump task (NTP/TestWiFi/TFTP) is actually active.

**Why:** Current reset path in `main.c` calls `NetworkPump_RequestAbortAll()` after a confirmed low pulse on `nRESET_PIN`. That abort API is global: it aborts all sessions, including Uthernet/MACRAW paths used by ADTPro. During U2-only traffic this can terminate an otherwise active transfer.

**What we changed:**

- Added `NetworkPump::HasActiveLegacyOperation()` in `pico/network_pump.h`.
- Added C bridge `IsNetworkPumpLegacyOperationActive()` in `pico/network.{h,cpp}`.
- In `pico/main.c` `ServiceAppleResetAbort()`, after low-hold and guard checks, abort now runs **only** when a legacy operation is active. If not, pending reset-abort is cleared and ignored.

**What we did not change:**

- No change to RECV/SEND pointer logic, MACRAW queueing, or lwIP polling cadence.
- No change to legacy-task abort behavior (NTP/TFTP/Test WiFi still abort on confirmed reset).

**Takeaway:** Reset-line noise or incidental reset pulses no longer globally tear down Uthernet sessions when ADTPro is the only active network workload.

**References:** `pico/main.c`, `pico/network.h`, `pico/network.cpp`, `pico/network_pump.h`.

**Architecture / open work:** Consolidated stack diagram, function contract table, prioritized TODO list, and per-item validation checklists live in `docs/Uthernet-II-stack-architecture-and-todos.md`.

---

## 1ak. May 2026 tcpdump correlation: mid-transfer stall (~17:13:16), not a silent stop at “block 37”

**Symptom:** ADTProETH disk transfer reported failing around “block ~37” with latest debug firmware.

**Evidence (`debug/tcpdump.txt`, segment ~17:13:12–17:13:20):**

- Baseline pattern in **`debug/tcpdump.txt`**: **192.168.0.245:6502** ↔ **192.168.0.154:6502** on port **6502**; short (**~9 B**) vs large (**~520–527 B**) UDP alternates in the bulk phase **as captured**.
- **Session intent (operator-confirmed):** Data flow **Mac → Apple IIc**, **inbound** to the Uthernet emulation — i.e. ADTPro **Send** (`CommsThread.sendDiskWide()` / `sendPacketWide()` on the Mac), Apple side **`GETREQUEST` / `RECVBLKS`** in `ethproto.asm`: bulk image bytes arrive **from the network into** the emulated W5100 RX path.
- **IP labeling vs. tcpdump:** For **Send**, one expects **large** UDP **toward the Apple’s IP** (dst = IIc). The saved capture shows **large** UDP with **src 192.168.0.154, dst 192.168.0.245**, which **by IP headers alone** reads like **.154 → .245** (opposite of “Mac=.245, IIc=.154”). Possibilities: **Mac was actually .154** for that run, **IIc was .245**, or this **pcap** is not the same session / interface semantics differ. **Future captures:** annotate **which address is the Mac** vs **IIc** beside the file.
- First clear anomaly: **17:13:16.652904** — first shortened **large** payload (**501 B**), then **333 B**; **~721 ms** until the next short (**9 B**) datagram (**17:13:17.484506**). That spacing matches **timeout / retry**, not the usual ~100–120 ms cadence.
- After the gap, more large payloads (509, 550, 410, 504×3 B), then **6 B** and **5 B** short packets — consistent with **protocol wind-down / error handling**.
- Counting **large** UDP payloads in the **500–532 B** range from the first full-speed bulk segment (~**17:13:12.456**) yields **about 25** before the **501 B** anomaly — **not** necessarily 37 disk blocks (**BAOCNT**, RLE length variation, or UI counting).

**UART retest (`debug/2026-05-02 17-34-57 FT232R USB UART #1.log`, session from ~18:03 local, firmware **`2026-05-02 21:58:01 UTC`):** Operator reports **no** listing-timeout this run; **Send** still fails mid-transfer (~“block 37”). **`[u2m]`** shows steady **MACRAW rx len ~546–592 B** from ~**18:03:25** through **last ~546 B ~18:03:33.484**, then **only small** RX (≤117 B); **`[u2udp] mq_drop=0`**. **~4 s** gap (**~18:03:34.4 → ~18:03:38.4**) with sparse frames — **mid-transfer stall**, same **class** as §1ak tcpdump anomaly, distinct from directory-parse host timeout.

**UART (`debug/2026-04-28 16-06-53 FT232R USB UART.log`, May 2 ~17:13):** Sampled **`[u2udp] … mq_drop=0`** lines give **no** evidence of MACRAW TX ring drops in this capture window.

**tcpdump “bad udp cksum” on large datagrams:** Often indicates **checksum 0 / offload** or capture verification quirks; **not** treated as proven corruption without another check.

**What we did not prove:** One-to-one mapping from Apple II block number to a particular Ethernet packet without a **sequence field** in firmware or host logs.

**Cross-check — parallel ADTPro tree** (`…/adtpro`, sibling of MegaFlash):

| Topic | Source | Fact |
|--------|--------|------|
| Host→Apple disk send | `CommsThread.sendDiskWide()` → `sendPacketWide()` | After handshake ACK **0x06**, host loops **`block = 0 .. length-1`**, batches up to **`blocksAtOnce`** (from client **BAOCNT**), builds envelope **`0xC1`, decompressed length `blocksAtOnce*512`, `0xD3`, check, start block lo/hi**, then **RLE** payload + **16-bit CRC**; waits for wide ACK whose payload must confirm **`block + blocksAtOnce`**. Progress bar: **`setProgressValue(block)`** in **block** units (not half-blocks; unlike narrow **`sendDisk()`** which uses **half-blocks**). |
| Client BAOCNT | `prodos/ethernet/ethconfig.asm`: **`PBAO`** default indexes **`BAOTbl`**: **`.byte 1,2`** | Default index **0 → 1 block per wide packet** unless the user picks the **2** option — then each UDP carries **2 disk blocks** (wire packet count ≈ **half** block count). |
| Apple receive path | `ethproto.asm` **`GETREQUEST`** (+ **`RECVBLKS`**) | Client sends **`CHR_G`** and **BAOCNT** in the request; receive loop uses **`PUTACKBLK`** / **`RECVWIDE_REPLY`** for acknowledgements. |

**UDP payload size:** **`sendPacketWide()`** size is **not** fixed at ~522 B — RLE shrinks or expands vs raw **512×blocksAtOnce** bytes, so tcpdump **length** is only a loose proxy for “one block.”

**Next steps:** Tag **Mac vs IIc IPv4** next to **`tcpdump`** output; low-rate **block index** logging on firmware RX path or ADTPro **`sendPacketWide`** traces; revisit **P0-1** in `docs/Uthernet-II-stack-architecture-and-todos.md`.

**References:** `debug/tcpdump.txt`, `debug/2026-04-28 16-06-53 FT232R USB UART.log`, `docs/Uthernet-II-stack-architecture-and-todos.md`; ADTPro: `adtpro/src/org/adtpro/CommsThread.java`, `adtpro/src/client/prodos/ethernet/ethproto.asm`, `adtpro/src/client/prodos/ethernet/ethconfig.asm`.

---

## 1am. ADTPro Send ~block 37: how we **find** root cause and **fix** it (not “live with 30 blocks”)

**Position:** The goal is **not** to cap transfers — it is to **measure** where bytes are lost or stalled, then **remove that bottleneck**. “Same block every time” usually means **same cumulative stress** on a **finite buffer / loss path**, not a magical constant.

### A. Falsifiable hypotheses (pick off with evidence)

| Hypothesis | If true, you would see… | Primary falsification |
|------------|-------------------------|------------------------|
| **H1 — MACRAW RX ring overflow / drop** | Non-zero **`reject_no_room`** (or **`reject_oversize`**) during the stall window | **`[u2macraw-rx]`** line (UART every 5 s with **`[u2udp]`**) shows counters climbing during Send; correlates with stall |
| **H2 — UDP `u2_push_rx` drops** | Lost datagrams without MACRAW reject | Instrument **`u2_push_rx`** return-0 count (not done yet); **`mq_drop`** already stays 0 in traces |
| **H3 — wrong RX capacity for socket 0** | Stall when **`receive_size`** too small for sustained bulk | Log **`u2_sockets[0].receive_size`** at MACRAW OPEN (or assert ≥ N KiB after ip65 writes RMSR) |
| **H4 — host/protocol** | Clean firmware counters; failure only with one BAOCNT/disk | ADTPro trace / tcpdump shows ACK mismatch without **`reject_*`** |

### B. Instrumentation now in firmware

- **`pico/uthernet2.c`** **`u2_push_rx_macraw`:** atomics **`enqueue_ok`**, **`reject_no_room`** (lost after **both** ring refusal **and** staging full), **`reject_oversize`** (**`2+len > receive_size`**). Supersedes older **`discard_*`** / **`reject_ring_*`** names (§1aq).
- **`pico/uthernet2_net.cpp`** **`U2_Net_Poll`:** every **5 s**, UART prints **`[u2macraw-rx] enqueue_ok=… reject_no_room=… reject_oversize=…`** with **`[u2udp]`**.
- **`U2_GetMacrawRxStats`** in **`uthernet2.h`** (three out-parameters).

**How to read one transfer:** Run Send until failure; snapshot last **`[u2macraw-rx]`** before/after stall. **Any sustained increase in `reject_no_room` or `reject_oversize`** → **H1 confirmed** — remediation is **ring sizing / host drain rate / Wi‑Fi burst**, not ADTPro.jar.

### C. Remediation paths (after hypothesis)

| Outcome | Concrete remediation |
|---------|----------------------|
| **H1 confirmed** | **Increase** effective **`receive_size`** for socket 0 (ip65 **RMSR** layout / W5100 memory map): give MACRAW **more KiB** so inbound bulk fits **II drain lag**. **Ring** path is **W5100-parity** (no head discard); **lwIP staging** (§1ao) still absorbs bursts before the ring. |
| **H1 ruled out, H3 suspected** | Verify **actual** **`receive_size`** after init; fix **`set_rx_sizes`** / defaults if ip65 expects **8 KiB** but emulation applies **4 KiB**. |
| **Host-side workaround only** | Lower Mac send rate / smaller batches (**BAOCNT**) — **diagnostic**, not the product fix. |

### D. Listing works, Send never starts (host timeout before bulk)

ADTPro **directory listing** and **disk Send start** are different exchanges (listing vs handshake / first **`CHR_G`** / ACK **0x06** path). Both ride **inside MACRAW Ethernet frames** for typical ip65 setups — **`u2_push_rx` UDP drops** are usually **irrelevant** (`[u2udp-rx] drop_ring_full` should stay **0**). Timeouts **before** the first block usually mean a **small** inbound frame was **lost or discarded** (`discard_recv` / **`reject_*`**) or the host never sees the II’s reply — treat **`[u2macraw-rx]`** during **only** the failed “Start transfer” attempt as decisive.

### E. Operator procedure (minimal)

1. Flash latest Debug UF2; confirm **`Firmware build:`** on UART.
2. Reproduce Send failure once; save UART from boot through failure.
3. Check **`[u2macraw-rx]`** and **`[u2udp-rx]`**: if **`reject_no_room`** / **`reject_oversize`** moved during the run → open issue **ring pressure** with numbers; if **`drop_ring_full`** rises while MACRAW stays clean → investigate **UDP pcb** path; if **all zero** → **host-side ADTPro logging** / **tcpdump** on the handshake window.

**References:** `pico/uthernet2.c` (`u2_push_rx_macraw`, §1aq), `pico/uthernet2_net.cpp` (`U2_Net_Poll`), `pico/lwipopts.h` (pool sizes — secondary suspect after MACRAW stats).

---

## 1al. P0-3: atomic RX pointers + single publish of `sn_rx_wr`

**Requirement:** Core 0 (lwIP → `u2_push_rx` / `u2_push_rx_macraw`) increments the RX write pointer; core 1 (bus → `get_rx_rsr`, RECV) reads **RX_RSR** and consumes. Without ordering, core 1 could observe **`sn_rx_wr`** advanced before all **`u2_memory`** stores for that datagram were visible (MP coherence gap).

**What we did:** In `pico/uthernet2.c`, `sn_rx_rd` / `sn_rx_wr` are **`_Atomic uint16_t`**. **`get_rx_rsr()`** uses **`memory_order_acquire`** loads; **`u2_push_rx`** and **`u2_push_rx_macraw`** compute the next write offset locally, write the ring bytes, then **`atomic_store_explicit(..., memory_order_release)`** once per accepted enqueue. **`SN_CR_RECV`** uses **`release`** stores to **`sn_rx_rd`** (MACRAW ingress no longer calls **`u2_socket_discard_rx`** — see §1aq).

**What we did not do:** Locking on the bus hot path; reorder **DATA-port** reads vs RSR (add instrumentation if stalls remain).

**Follow-up (directory-parse / host-timeout hypothesis):** `u2_push_rx` takes an initial `(sn_rx_rd, sn_rx_wr)` snapshot for free-space. Core 1 can **RECV** (advance `sn_rx_rd`) immediately after that snapshot; the ring then has room, but core 0 could still **return 0** for UDP (atomic datagram, no partial queue) and **drop** the inbound datagram — ADTPro host waits, times out. **Mitigation:** if UDP would be dropped for “not enough free bytes,” **reload** `sn_rx_rd`/`sn_rx_wr` once and recheck; same **once** for TCP when `free_bytes == 0`. This does not replace single **release** publish of `sn_rx_wr` after ring writes.

**Why P0-3 could feel worse without this:** Stricter atomic visibility makes “reject full” decisions **consistent** with the snapshot; torn non-atomic reads could occasionally **over-report** free space (risk overflow) or behave differently under race — neither is desirable, but **consistent reject** without reload increases **deterministic drop** when core 1 frees space between loads.

**References:** `docs/Uthernet-II-stack-architecture-and-todos.md` (P0-3 row + checklist); `pico/uthernet2.c`.

---

## 1an. ADTPro Send remediation implementation (2026-05): MACRAW discard policy, TX_RD honesty, drain budget

**Requirement:** Execute §1am plan branches without blaming validation software: reduce inbound loss under bulk MACRAW RX, align **`Sn_TX_RD`** with bytes actually accepted by lwIP, and drain deferred MACRAW TX faster under queue pressure.

**What we did:**

1. **`u2_push_rx_macraw` (`uthernet2.c`):** When a new frame does not fit, **drop oldest complete MACRAW records** (`u2_macraw_discard_head_frame`) in a bounded loop before falling back to **`u2_socket_discard_rx`** (full wipe). Counter **`discard_head`** (`u2_macraw_rx_discard_head`); **`discard_recv`** still counts full wipes. **`seq`** (`u2_macraw_rx_seq`) increments on each successful enqueue — correlates UART with tcpdump.
2. **`send_data`:** **TCP only:** **`Sn_TX_RD`** advances by **`U2_Net_SendTcp`** accepted bytes (partial on **`tcp_write`** failure). **UDP and MACRAW:** **permissive** advance to **`TX_WR`** after every SEND (§1y) — strict lwIP-ok gating caused mid-transfer “block ~37” style failure **without** host timeouts; real stacks assume pointer progress.
3. **`u2_send_macraw_core0`:** Returns **`bool`**; **`DHCP BOOTP chaddr`** patch is invoked here (was previously unused on this path — see architecture §P2-2). **`linkoutput`** **`ERR_OK`** required before **`U2_MonNetMacrawTx`**.
4. **`U2_Net_Poll`:** Adaptive MACRAW drain (**4 / 8 / 16** frames per poll by queued depth). **Dequeue only after successful send** so **`linkoutput`** failure retries next poll.
5. **UART:** **`[u2macraw-rx]`** now prints **`seq`**, **`discard_head`**, **`discard_recv`**, …

**What we did not do:** Increase **RMSR** beyond ip65’s writes (still validate **H3** if stalls persist). **P0-1** “honest” UDP/MACRAW **`TX_RD`** is **not** applied when it conflicts with §1y liveness; **TCP** remains the strict path.

**References:** `pico/uthernet2.c`, `pico/uthernet2_net.{h,cpp}`, `pico/uthernet2.h`; §1am.

---

## 1ao. MACRAW RX staging FIFO (ADTPro Send still ~block 37 after §1y TX_RD)

**Symptom:** Send still failed near the **same storage block** after restoring **UDP/MACRAW permissive `TX_RD`** — points away from outbound pointer gating.

**Why:** With **RMSR** giving socket 0 only **4 KiB** RX (typical ip65 **0x0A** layout), **Wi‑Fi can deliver Ethernet frames faster** than the II drains the emulated ring. Discarding (**head drop** / **reject**) loses **in-order bulk** and surfaces as a **repeatable block index**, not necessarily host “timeout.”

**What we did:** **`u2_push_rx_macraw_into_ring`** holds the prior ring logic; if it fails, **socket 0** frames are copied to a **16×1518 B FIFO** (`U2_MACRAW_STAGE_DEPTH`). **`U2_MacrawRxStagingPoll()`** runs at start and end of **`U2_Net_Poll`** and moves staged frames into the ring when space exists. UART: **`[u2macraw-stage] enq= flush= staging_full=`**. Counters: **`U2_GetMacrawRxStagingStats`**.

**What we did not do:** Change visible **RMSR** or ip65 buffer math (would risk host/emulator mask mismatch).

**Follow-up (still ~block 36–37):** **`PicoW_ServiceCore0IpcAndNetwork`** had **`multicore_fifo_pop_timeout_us` before `U2_Net_Poll`** with **`50 ms`** timeout in **`core0Loop`’s NTP wait loop** — core 0 could go **tens of ms** between **`cyw43`/lwIP/staging** service while Wi‑Fi kept delivering MACRAW. **Fix:** run **`U2_Net_Poll`/`U2_MonPollFlush` first**, then FIFO wait; **`core0Loop`** FIFO timeout **50 ms → 1 ms** (`main.c`). Aligns with earlier SESSION_LOG intent that had drifted from code.

**Follow-up (timeouts in file-selection UI + ~block 37):** **1 ms** idle FIFO wait still caps core‑0 servicing at **~1 kHz** when the FIFO is empty — marginal for **small MACRAW control** exchanges (selection) and **bulk**. **Updates:** **`core0Loop`** idle wait **1 ms → 100 µs**; **`U2_Net_Poll()` twice** per service tick; **MACRAW staging depth 16 → 32**; staging drain **32 → 64** attempts per **`U2_MacrawRxStagingPoll`**. **Operator note:** ADTPro **BAOCNT** **2 blocks/packet** advancing failure **37 → 38** fits **wire packet count / burst size** better than a magic disk-block constant — still stress on **ingress FIFO + 4 KiB W5100 RX**.

**Regression (host timeout before list parse / initial connect):** **Double `U2_Net_Poll`** per wake + **100 µs** FIFO spin correlated with **host** waiting on **II** for early ADTProETH path. **Reverted** to **single `U2_Net_Poll`**; **idle FIFO 100 µs → 500 µs** (~**2 kHz** max). **Kept:** poll **before** FIFO block, **32**-frame MACRAW staging, **64**-step staging drain. *Reasoning:* double `NetworkPump`/lwIP service per tick may have reordered or starved a path the listing handshake needs; 100 µs may have interacted badly with CYW43/timing on some runs.

---

## 1aq. MACRAW RX W5100 parity (2026-05): no head discard in ring; lwIP staging **outside** emulated RX

**Requirement (W5100 register file):** If the **complete** MACRAW record (2-byte BE **wire_len** + payload) does not fit in the **free** emulated receive memory, that record is **not** written to **`u2_memory`**; **existing** ring bytes stay untouched (no head eviction, no **RX_RD→WR** wipe). A separate **core0 staging FIFO** (§1ao) may still hold a copy from lwIP until **`U2_MacrawRxStagingPoll`** can move it into the ring — that FIFO is **not** the W5100’s internal buffer; it restores MegaFlash timing without changing **RSR** math.

**Why:** ip65/ADI were written for silicon that **drops the newest** frame if the **chip** RX is full. §1an’s **head discard** / **full wipe** in **`into_ring`** was non-silicon; **removed**. **Staging** is the pre-§1aq bridge from **CYW43/lwIP** to the emulator when the II has not **RECV**’d yet — **not** a second W5100 buffer.

**What we did:** **`u2_push_rx_macraw_into_ring`** (`uthernet2.c`) checks **`used + total <= size`** (with one **rd/wr** reload vs §1al TOCTOU); if not, returns **false** without mutating the ring — **no** head eviction, **no** **`RX_RD→WR`** wipe (**removed** **`u2_macraw_discard_head_frame`** / **`u2_socket_discard_rx`** from MACRAW). **`reject_oversize`** counts **`2+len > receive_size`** inside **`into_ring`**. **`reject_no_room`** increments only when socket **0** cannot **`macraw_stage_enqueue`** after **`into_ring`** fails (**staging full** = combined ingress loss). **Staging FIFO** (`32×1518 B`) and **`U2_MacrawRxStagingPoll`** restored: lwIP→MegaFlash absorb bursts **outside** the emulated W5100 RX memory — **does not** alter parity **inside** **`sn_rx_rd`/`sn_rx_wr`** geometry; **UDP `u2_push_rx`** unchanged.

**What we did not do:** Restore §1an **head discard** or **full-ring wipe** inside **`into_ring`**. **`u2_netif_input_wrapper`** duplicate **`netif->input`** is §**1ar** / **`483e8da`** semantics — not ring parity.

**Takeaway:** **`reject_no_room`** (after staging) means lost inbound Ethernet when MACRAW is active; staging mirrors pre–§1aq MegaFlash servicing behavior without falsifying W5100 ring rules.

**References:** `pico/uthernet2.c`, `pico/uthernet2.h`, `pico/uthernet2_net.cpp`; §1am (hypothesis table: interpret **`reject_no_room`** like **`reject_*`**); §1an–§1ao (superseded ingress policy).

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
- `main.c`: In Debug, UART stdio is initialized (**460800** baud via `uart_set_baudrate(uart_default, 460800)` after `stdio_uart_init`) and the bus loop (Core 1) is **always** started, even if the Apple is not connected, so you can plug in the Apple later and test. In Release, the bus loop is started only when `IsAppleConnected()` is true.
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

**What we did:** Factor **`PicoW_ServiceCore0IpcAndNetwork(fifo_timeout_us)`**: optional **`multicore_fifo_pop_timeout_us`**, then **`U2_Net_Poll()`** (hooks + **`NetworkPump_PollOnce()`**), **`U2_MonPollFlush()`**, then IPC dispatch. **`core0Loop()`** inner loop uses **50 ms** FIFO timeout. (A trial **poll-before-FIFO** + **500 µs** timeout to help ADTPro MACRAW was **reverted** — it regressed **MegaFlash** NetworkPump/TFTP-style traffic; ADTPro needs a different lever, e.g. §**1au** / dedicated scheduling, not this reorder.) The Pico W USB-terminal **`while (true)`** calls it with **0** each iteration (non-blocking) and replaces the 1 s idle sleep with **1 ms** sleeps so **`cyw43_arch_poll`** runs continuously during idle.

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

## 10j. TCP session hang after connect (Contiki browser / telnet65) — pbuf + TX_RD fixes

**Symptom (2026-06-21):** UDP paths work (DNS, NTP/datetime, short queries). TCP connects (telnet65 prints “connecting … Ok”) but sustained sessions fail: Contiki browser hangs after HTTP connect attempt; telnet65 disconnects after a brief exchange. Pattern: **1–2 packet flows OK, multi-segment TCP fails**.

**Why (root cause — two bugs):**

1. **RX — partial ring accept vs whole-pbuf free:** §10i added TCP **partial** enqueue into the emulated RX ring and `tcp_recved(accepted)` for only those bytes — but `u2_tcp_recv()` still **`pbuf_free(p)`** for the **entire** lwIP segment. Bytes not copied into the W5100 ring were **destroyed without `tcp_recved`**, corrupting the byte stream and stalling the peer window. When the ring was **full**, `accepted==0` still freed the pbuf without `tcp_recved` — lwIP could not retain the segment for retry.

2. **TX — `Sn_TX_RD` always advanced:** `send_data()` called `U2_Net_SendTcp()` in a loop but always moved **`Sn_TX_RD → Sn_TX_WR`** even when `tcp_write()` returned **`ERR_MEM`**. The II believed bytes were sent; lwIP never queued them — server saw missing client data and closed or timed out.

**What we did:**

- **`u2_push_rx` (TCP):** **All-or-nothing** again — if `free_bytes < len`, return **0** (no partial slice).
- **`u2_tcp_recv`:** On **`accepted==0`**, return **`ERR_MEM`** **without** freeing the pbuf (lwIP **`refused_data`** retry). On success, **`tcp_recved(accepted)`** then **`pbuf_free`**.
- **`U2_Net_RecvConfirm`:** After host **RECV**, retry **`pcb->refused_data`** via **`u2_tcp_recv`** so ring space freed by the 6502 unblocks stalled segments without waiting for another wire packet.
- **`U2_Net_SendTcp`:** Returns **bytes accepted** by **`tcp_write`** (0 on reject).
- **`send_data` (TCP):** Advances **`Sn_TX_RD`** by **accepted bytes only**; leaves **`Sn_CR=SEND`** set when output stalls; **`U2_TryCompletePendingSends()`** (from **`U2_Net_Poll`**) retries until complete.

**What we didn’t do:** Did not change UDP/MACRAW TX pointer rules in this pass (MACRAW trampoline overwrite remains §5 P0-2).

**Takeaway:** W5100 emulation at the lwIP boundary must be **segment-atomic on RX** and **byte-honest on TX** — partial ring fills are fine for **host RECV** semantics, not for splitting a single **`tcp_recv` pbuf**.

**References:** `pico/uthernet2.c` (`u2_push_rx`, `send_data`, `U2_TryCompletePendingSends`), `pico/uthernet2_net.cpp` (`u2_tcp_recv`, `U2_Net_SendTcp`, `U2_Net_RecvConfirm`, `U2_Net_Poll`).

---

## 10k. Contiki slow + intermittent DNS after §10j TCP fix

**Symptom:** After §10j, Contiki browser **works** but is **very slow** (likely TCP retransmits). **telnet65**, **wget65**, and Contiki wget report **DNS timeout** intermittently; sometimes DNS succeeds and downloads work.

**Why:** §10j **`ERR_MEM` / `refused_data`** is correct but needs **timely core-0 lwIP service**. With Apple II connected, **`core0Loop`** waits up to **50 ms** between **`U2_Net_Poll`** calls. **`U2_Poll()`** on core 1 returns immediately (`get_core_num()!=0`). Stalled **`refused_data`** pbufs tie up **`PBUF_POOL`** and delay UDP DNS replies. §10j also ran **`U2_Net_RecvConfirm`** lwIP retry on **core 1** inside the bus **`SN_CR_RECV`** path — risky vs core-0 poll. **`U2_TryCompletePendingSends`** scanned **all** sockets with **`Sn_CR=SEND`**, not just stalled **TCP ESTABLISHED** (UDP completes on core 1).

**What we did:**

- **`IPCCMD_NET_WAKE`**: core 1 **`U2_Poll`** pushes FIFO wake (max **1/ms**) so core 0 polls during active II bus traffic.
- **`core0Loop`**: FIFO timeout **50 ms → 5 ms** as backstop.
- **`U2_Net_RecvConfirm`**: sets per-socket retry flag only; **`U2_Net_TcpRetryRefusedPending()`** runs on core 0 each **`U2_Net_Poll`** (also retries any socket with **`refused_data`**).
- **`U2_TryCompletePendingSends`**: only **TCP ESTABLISHED** pending SEND.
- **`send_data`**: return **-1** when **`data_len>0`** but mode not handled (do not false-clear **`Sn_CR`**).
- Hot path: shared **`u2_net_copy_buf[1536]`** under lwIP lock instead of **`std::vector`** / large stack arrays (RP2350 RAM budget).

**What we didn’t do:** Did not raise **`PBUF_POOL_SIZE`** (40 overflowed RP2350 RAM). Contiki page load may still be slower than real Uthernet II until host RECV cadence + ring sizing are tuned.

**References:** `pico/ipc.h`, `pico/main.c`, `pico/uthernet2.c` (`U2_Poll`), `pico/uthernet2_net.cpp`.

---

## 10l. Contiki wget checksum errors at ~1390-byte intervals (RX pointer geometry)

**Symptom:** Contiki **wget** succeeds on very small files; larger downloads show **checksum errors** at cumulative offsets **1162, 2552, 3942, 5332** (then **+1390** each time) and eventually **timeout**. **1390 = 2048 − 658** — strongly suggests **2 KiB RX ring** occupancy miscalculation after wrap or during concurrent **RX_RD** updates.

**Why:** In `pico/uthernet2.c`, **`sn_rx_wr`** was stored **masked** to the ring size (`& (size−1)`) and **`get_rx_rsr()`** masked both **RX_RD** and **RX_WR** before subtracting. After the first lap, masked **wr** can equal **rd** while the ring still holds unread bytes → **RSR under-reports** → **`u2_push_rx`** accepts TCP segments and **overwrites** unread data → periodic corruption. Separately, **core 0** reads **RX_RD** as a plain 16-bit pair while **core 1** writes **RX_RD0/1** as two byte stores → **torn reads** can also under-estimate **used** and cause the same overwrite pattern (documented in §1ai / §1al but had drifted out of code).

**What we did:**

- **`sn_rx_wr`**: keep as **full 16-bit monotonic** counter; **mask only** for **`u2_memory[base + (wr & mask)]`** indexing; **`release`** store once after all ring bytes written.
- **`read_rx_rd_coherent()`**: high/low/high retry on **Sn_RX_RD0/1** for **`get_rx_rsr` / free-space checks**.
- **`u2_rx_used_bytes()`**: **`used = (wr_off − rd_off + size) % size`** with **`wr_off = sn_rx_wr & mask`** and **`rd_off = RX_RD & mask`** (ip65 uses physical **`Sn_RX_RD`**; mask is ring offset). **`sn_rx_wr`** itself stays **monotonic** across laps.
- **`u2_push_rx` / `u2_push_rx_macraw`**: **TOCTOU reload** of used/free once when first check says full (§1al).
- **`u2_socket_discard_rx`**: write **full monotonic `sn_rx_wr`** to **RX_RD** registers (not masked offset).

**What we didn’t do:** Did not change **RMSR** sizing or TCP segment sizes; did not add **`sn_rx_rd` shadow** (coherent register read is enough for **RSR** while host updates **RX_RD** between **RECV**s).

**Takeaway:** W5100 RX occupancy is **`wr − rd` on monotonic 16-bit pointers**, not **`(wr & mask) − (rd & mask)`** — masking both sides breaks after the first buffer lap and matches periodic wget corruption.

**References:** `pico/uthernet2.c` (`read_rx_rd_coherent`, `u2_rx_used_bytes`, `u2_push_rx`, `get_rx_rsr`); §1ai, §1al (prior ADTPro pointer work).

---

## 10m. MegaFlash native NTP/DHCP failed when U2 MACRAW active (legacy primary)

**Symptom:** After §10k (frequent **`U2_Net_Poll`** + TCP **`refused_data`** retry), **MegaFlash native** **NTP** and **WiFi/DHCP** (**`ConnectWifi`**) stopped working reliably while the Apple II had **U2 MACRAW** open (ip65 init / Contiki / ADTPro path).

**Why:** One **STA netif** serves both stacks. **`BeginLegacyOperation`** runs **before** **`ConnectWifi`** in **`RunNTP` / `RunTestWifi` / `RunTFTP`**, but **`u2_netif_input_wrapper`** still copied **every inbound Ethernet frame** into the emulated W5100 ring **inside the lwIP `input` callback** while core 0 was in native **`cyw43_arch_poll` / DHCP**. That added core‑1-visible ring work and **`refused_data`** TCP retries on the same poll thread, starving or racing the native DHCP client. §10k also ran **`U2_Net_TcpRetryRefusedPending`** / **`U2_TryCompletePendingSends`** from the idle **`U2_Net_Poll`** path outside **`PollOnce`**, so **`RunNTP`'s inner `PollOnce` loop** did not drain **MACRAW TX** (§8 P0‑5) while still competing on ingress.

**What we did:**

- **`NetworkPump::IsLegacyOperationActive()`** — true during **`RunNTP` / `RunTFTP` / `RunTestWifi`**.
- **`u2_netif_input_wrapper`**: when legacy active, **forward to `u2_saved_netif_input` only** (no MACRAW ring copy on the input hook).
- **`U2_Net_ServicePoll()`** — **MACRAW TX drain always**; **TCP retry + pending SEND only when legacy inactive**; called from **`NetworkPump::PollOnce()`** so native **`RunX`** inner loops get MACRAW drain without U2 ingress/TCP competing during DHCP.
- **`U2_Net_Poll()`** → **`NetworkPump_PollOnce()`** only (single poll entry).

**What we didn't do:** Did not block **`U2_HandleBusAccess`** or close U2 sockets during native ops (Apple may still bit‑bang registers; only **shared lwIP ingress/TCP backpressure** is deferred).

**Takeaway:** MegaFlash native **must own the STA `input` hot path** during **`RunX`**; U2 MACRAW duplication belongs in the idle/coexistence window, not inside the same callback as native DHCP.

**References:** `pico/network_pump.{h,cpp}`, `pico/uthernet2_net.cpp` (`u2_netif_input_wrapper`, `U2_Net_ServicePoll`, `U2_Net_Poll`); §8 coexistence policy.

---

## 10n. Revert network stack to pre-checksum baseline (2026-06-21)

**Requirement:** User report — nothing worked reliably after §10j–§10m (TCP backpressure, poll wake, RX pointer / wget checksum, native/U2 coexistence). Revert **code** to last committed tree before those experiments.

**What we did:** `git restore` on **`pico/uthernet2.c`**, **`uthernet2.h`**, **`uthernet2_net.cpp`**, **`uthernet2_net.h`**, **`main.c`**, **`ipc.h`**, **`network_pump.cpp`**, **`network_pump.h`** → **`865d4d7`** (merge) baseline. Rebuilt **`pico2_debug/megaflash.uf2`**.

**Follow-up (§10l re-land):** User requested **§10l only** (RX pointer geometry) without §10j/k/m. Re-applied in **`pico/uthernet2.c`**: **`read_rx_rd_coherent`**, **`u2_rx_used_bytes`**, monotonic **`sn_rx_wr`** with mask-only indexing, TOCTOU reload, **`u2_socket_discard_rx`** writes **`receive_base + wr_off`** to **RX_RD**. **Did not** change **`uthernet2_net.cpp`** (TCP partial accept remains). Rebuilt **`pico2_debug/megaflash.uf2`**.

**Follow-up (§10j re-land, after §10l commit `7adb4ee`):** Isolated **§10j** on top of §10l — **`u2_push_rx` TCP all-or-nothing**; **`u2_tcp_recv`** **`ERR_MEM`** without freeing pbuf when ring full; **`U2_Net_RecvConfirm`** sets retry flag, **`U2_Net_TcpRetryRefusedPending`** on core 0; **`U2_Net_SendTcp`** returns accepted bytes; **`send_data`** honest **`Sn_TX_RD`** + pending **`Sn_CR=SEND`**; **`U2_TryCompletePendingSends`**. **Did not** land §10k (no **`IPCCMD_NET_WAKE`**). Rebuilt **`pico2_debug/megaflash.uf2`**.

**Validation (user, post-§10l re-land):** Contiki browsing **much faster**; MegaFlash native networking **OK**; Contiki **wget ~100 KiB** before timeout (was failing much earlier with **regular** checksum errors every ~1390 B). Checksum errors **still occur** but **no longer at fixed intervals** — consistent with §10l fixing ring geometry overwrite; remaining corruption likely **§10j** partial TCP segment accept and/or **§10k** poll/`refused_data` stall on long transfers.

**What we did not do:** Did not revert **`docs/`** §10j–§10m prose (historical record of attempted fixes; **not in firmware** until re-applied). ROM disk already reverted separately (§SESSION_LOG 2026-06-21).

**Takeaway:** §10j–§10m remain design notes for a future **selective** re-land; **§10l** is now in firmware again as an isolated change.

**References:** git **`865d4d7`**; summary table rows §10j–§10m marked superseded by this revert except §10l re-land.

---

## 10o. §10j collateral: DNS first-try fail, wget connect timeout, telnet immediate disconnect

**Symptom (user, post-§10j on §10l `7adb4ee`):** Contiki browser **very good** (better than §10l alone). **DNS** fails on first try more often than before §10j. Contiki **wget** resolves IP but **cannot establish TCP** (timeout from the start). **ip65 telnet65**: “connecting to &lt;IP&gt; OK” then **immediate disconnect**.

**Why:**

1. **§10j without §10k:** TCP **`refused_data`** ( **`ERR_MEM`** when RX ring appears full) holds **pbufs** until core 0 retries **`U2_Net_TcpRetryRefusedPending`**. With Apple connected, **`core0Loop`** only polled lwIP every **~50 ms**; **`U2_Poll`** on core 1 is a no-op for lwIP. That starves UDP/DNS and delays TCP handshake/recv retry.
2. **Stale `sn_rx_wr` on socket reuse:** **`sn_rx_wr`** was not reset on **OPEN** / **CLOSE**. After a prior session, **`u2_rx_used_bytes`** can report a nearly full ring on a fresh connection → first inbound TCP segment gets **`ERR_MEM`** → peer may RST or host sees **CLOSED** (telnet “connect OK then disconnected”).
3. **`send_data` during SYNSENT:** Honest TX returns **-1** when **`data_len>0`** but status is not **ESTABLISHED**, leaving **`Sn_CR=SEND`** stuck until poll — some clients issue **SEND** before **ESTABLISHED**.

**What we did (§10k + §10o, no §10m):**

- **`IPCCMD_NET_WAKE`**: **`U2_RequestCore0NetPoll()`** from **`U2_Poll`** (core 1, rate-limited **1/ms**); **`core0Loop`** FIFO wait **50 ms → 5 ms**.
- **`u2_socket_reset_rx`**: zero **`sn_rx_wr`** and set **`Sn_RX_RD`** to **`receive_base`** on successful **OPEN** and on **CLOSE/DISCON**.
- **`send_data`**: **SYNSENT** / **SOCK_INIT** with pending TX returns **0** (clear **`Sn_CR`**, data stays for **SEND** after **ESTABLISHED**).
- **`u2_tcp_connected_cb`**: on **ESTABLISHED**, **`U2_TryCompletePendingSends`** on core 0 or **`U2_RequestCore0NetPoll`** from core 1.
- **`u2_tcp_recv`**: **`ERR_MEM`** only when **`copied>0 && accepted==0`** (not on **`copied==0`**).

**What we didn’t do:** §**10m** native-op MACRAW ingress gate (user’s MegaFlash native path was OK on §10l; avoid re-introducing NTP/DHCP regressions until validated).

**Takeaway:** §**10j** segment-atomic RX is necessary for wget integrity but **requires** timely core-0 poll (**§10k**) and **fresh RX geometry per socket** — otherwise connect-path TCP and DNS regress while bulk browse can still look fast.

**References:** `pico/ipc.h`, `pico/main.c`, `pico/uthernet2.c` (`U2_Poll`, `u2_socket_reset_rx`, `send_data`), `pico/uthernet2_net.cpp` (`u2_tcp_connected_cb`, `u2_tcp_recv`).

---

## 10p. wget/telnet connect timeout persists after §10o (browser OK)

**Symptom (user, post-§10o):** Contiki **browsing works**. Contiki **wget** still resolves host then **TCP connect times out**. **telnet65** unchanged (connect OK → immediate disconnect).

**Why:** §10o kept §10j **`ERR_MEM`** when the emulated RX ring could not accept a whole segment. That holds **`refused_data`** pbufs and can stall **new** TCP handshakes even with §10k poll wake, while **browser** sockets stay warm on existing sessions. Separately, §10o **cleared `Sn_CR` without advancing `TX_RD` during SYNSENT** — Contiki/ip65 often **SEND** before **ESTABLISHED**; §10l advanced **`TX_RD`** (discarding mistimed payload) so the host re-queues HTTP after connect. §10o **OPEN-time `RX_RD` reset** may also have raced Contiki init ordering.

**What we did:**

- **`u2_tcp_recv`**: back to §10l — **`tcp_recved` only when `accepted>0`**, always **`pbuf_free(p)`** (no **`ERR_MEM`**). **Kept** §10j **all-or-nothing** **`u2_push_rx`** (no partial ring slice → no silent tail drop).
- **`send_data`**: restore §10l **SYNSENT/INIT** behavior — advance **`TX_RD→TX_WR`** without lwIP send; **ESTABLISHED** path stays §10j honest **`tcp_write`**.
- **`u2_rx_rd_ring_offset`**: **`(physical_rd − receive_base) & mask`** for **`u2_rx_used_bytes`**.
- **`u2_socket_reset_rx`**: on **CONNECT** (success) and **CLOSE** only — not **OPEN**.
- **`U2_TcpFlushPendingTx`**: on **`u2_tcp_connected_cb`** and each **`U2_Net_Poll`** when **ESTABLISHED** and **`TX_RD≠TX_WR`**.

**What we didn’t do:** Did not revert §10k poll wake or §10l ring geometry.

**Takeaway:** Segment-atomic **ring enqueue** and **honest ESTABLISHED TX** can coexist with §10l **recv free** semantics; **`ERR_MEM`** on the hot path is too aggressive for multi-socket Contiki + ip65 bring-up.

**References:** `pico/uthernet2.c`, `pico/uthernet2_net.cpp`, `pico/uthernet2.h`.

---

## 10q. Revert §10j–§10p to §10l only — ip65 probe / Contiki ipconfig broken

**Symptom (user, post-§10p):** **ip65** no longer recognizes Uthernet emulation (“device not found”). **Contiki** cannot run **ipconfig** (fails before networking). Browsing/TCP experiments moot if W5100 chip probe fails.

**Why:** §10j–§10p bundled changes to **`u2_tcp_recv`**, **`send_data`**, **`U2_Poll`** FIFO wake, **`u2_socket_reset_rx`**, and poll-side **`U2_TcpFlushPendingTx`** regressed **bring-up** — the ip65 **`w5100.s`** RTR XOR probe and Contiki init path depend on stable, low-latency **`$C0C4–$C0C7`** register reads without side effects from core-0 networking on the bus hot path.

**What we did:** **`git restore --source=7adb4ee`** on **`pico/uthernet2.c`**, **`uthernet2_net.cpp`**, **`uthernet2.h`**, **`uthernet2_net.h`**, **`main.c`**, **`ipc.h`** — firmware back to **§10l only** (RX ring geometry). Rebuilt **`pico2_debug/megaflash.uf2`**.

**What we didn’t do:** Did not delete §10j–§10p design notes; future TCP fixes must be re-landed **one at a time** with ip65 probe + Contiki ipconfig as first gate.

**Takeaway:** Never ship bundled U2 TCP/poll changes without passing **ip65 detect** and **Contiki ipconfig** before wget/telnet tuning.

**References:** git **`7adb4ee`**; summary table rows §10j–§10p superseded in firmware by this revert.

---

## 10r. telnet65 pcap — `sensoroni_onion_1005.pcap` (onionfu.com / 71.63.243.155:6502)

**Capture:** 10 packets, ~427 ms, STA **192.168.0.203:26135** → **71.63.243.155:6502** (telnet65 “connect then immediate disconnect”).

**Timeline (seconds from first SYN):**

| Δt (ms) | Direction | Flags | Notes |
|--------:|-----------|-------|-------|
| 0 | → server | **SYN** | `win=1460` (one MSS), TTL 64 |
| 86 | ← server | **SYN-ACK** | `mss 1460`, `win=64240` |
| 152 | → server | **SYN** (retry) | Same seq — **SYN-ACK not ACKed within ~66 ms** |
| 154 | → server | **RST** | TTL **255**, `win=41005` — **abort before handshake done** |
| 239 | ← server | SYN-ACK (retry) | |
| 241 | → server | **ACK** | Handshake completes |
| 289 | → server | **RST** | TTL 255 again — **~47 ms after ESTABLISHED** |
| 325 | ← server | RST | |
| … | | stray ACK/RST | tail retransmits |

**What this means:**

1. **Not a DNS or routing problem** — SYN reaches the server and SYN-ACK returns.
2. **Handshake latency** — the Pico does not ACK the first SYN-ACK for **~66 ms**, so lwIP retransmits SYN. That matches **core 0 `U2_Net_Poll` only every ~50 ms** while the II bus runs on core 1 (§10k hypothesis).
3. **We abort the connection** — RST packets from **192.168.0.203** use **TTL 255** (vs TTL 64 on SYN/ACK), consistent with **lwIP `tcp_abort` / CLOSE** after the emulated W5100 socket goes **CLOSED**, not a middlebox drop.
4. **No application data** — the server never sends a telnet banner in this window; the session dies from **RST ~47 ms after the ACK that completes the handshake**, matching “connecting … OK” then “disconnected” if the UI reports success at **ESTABLISHED** (or a brief SYNSENT window before the first RST).
5. **`win=1460` on SYN** — receive window is only one segment; worth checking later but not the primary failure here.

**Next captures / correlation:**

- Same test with UART **`[u2m]`** socket **OPEN / CONNECT / SR / CLOSE / RECV** lines aligned to wall clock.
- Optional: wget or browser TCP to the same host for comparison (does server send data before we RST?).
- If §10l baseline is confirmed on hardware, re-land **§10k poll wake only** (no §10j recv) and re-capture — expect first SYN-ACK ACK within one poll period and no pre-handshake RST.

**References:** user file `sensoroni_onion_1005.pcap`; `pico/main.c` (`core0Loop` 50 ms FIFO); §10k/§10q notes.

---

## 10s. Contiki browser pcap — `sensoroni_onion_1006.pcap` (asimov.applefritter.com)

**Capture:** 10 packets, two load attempts (**22:52:33** and **23:08:13**), STA **192.168.0.203:1026** → **66.59.109.26:80**. Contiki browser (not wget): `GET /` with `Host: asimov.applefritter.com`, `User-Agent: Contiki/3.x`.

**Per-attempt pattern (identical both times):**

| Δt (ms) | Direction | Flags | Notes |
|--------:|-----------|-------|-------|
| 0 | → server | **SYN** | **`seq=0` ISN**, `win=1460`, `mss 1460` |
| 16 | ← server | **SYN-ACK** | Fast reply |
| 58 | → server | **RST** | TTL **255** — **handshake never ACKed** |
| 418 | → server | **PSH** | **HTTP GET** (124 B) `seq 1:125` on **same 4-tuple** |
| 434 | ← server | **RST** | Dead connection |

**What this means:**

1. **DNS/routing OK** — TCP SYN reaches a live web server.
2. **Handshake never completes on the wire** — unlike telnet65 (§10r), there is **no ACK** at all; we go **SYN → SYN-ACK → RST**.
3. **~58 ms from SYN-ACK to RST** — strongly suggests the **II stack times out CONNECT** and issues **CLOSE** before core 0 processes SYN-ACK and sends ACK (poll latency + W5100/lwIP state gap). SYN-ACK arrives in 16 ms; the bottleneck is **not** WAN RTT.
4. **Split-brain after RST** — **~344 ms later** Contiki still **SENDs HTTP** on the same **src port 1026** as if the socket were up; server correctly RSTs. Explains browser “tries to connect / hangs” without wget involvement.
5. **`seq=0` on SYN** — both attempts use **ISN 0** (visible in hex). Unusual for lwIP; worth correlating with `tcp_connect` / PCB init (may be Contiki-visible only if we mirror oddly; still flag for firmware review).
6. **`win=1460`** again — one-MSS receive window on SYN (same as telnet §10r).

**Contrast with telnet65 (`sensoroni_onion_1005.pcap`):**

| | telnet65 | Contiki browser |
|--|----------|-----------------|
| SYN-ACK → ACK | Delayed (~66 ms), eventually ACKs | **Never ACKs** |
| RST timing | ~2 ms after SYN retry; second RST ~47 ms after ACK | **~58 ms after SYN-ACK** |
| App data after RST | None | **HTTP GET on dead session** |

**Firmware implication:** Priority fix is **timely SYN-ACK processing** (§10k-style core-0 poll wake) **without** breaking ip65 probe (§10q). Second: on **CLOSE/RST**, ensure **W5100 `Sn_SR` and TX path** cannot **SEND** afterward (Contiki ghost HTTP).

**Next:** UART `[u2m]` for **CONNECT → SR → CLOSE/SEND** aligned to pcap; optional §10k-only trial + re-capture.

**References:** user file `sensoroni_onion_1006.pcap`; §10r; `pico/uthernet2_net.cpp` (`U2_Net_ConnectTcpEx`, `u2_tcp_connected_cb`).

---

## 10t. Long session pcap — `sensoroni_onion_1012.pcap`

**Capture:** ~**10 hours** (11:37 → 21:22), **~1360** packets — mostly **ARP** (~477 entries). STA **192.168.0.203** (CYW43 **88:a2:9e:48:22:7a**). Includes Contiki HTTP attempts, telnet65, MegaFlash **NTP**, DHCP, and background ARP.

**Note:** Path `MegaFlash-TF/pico-sdk-cache/.../btstack_uart_block_windows.c` is **unrelated** (Windows BTstack in SDK cache); analysis is from the pcap only.

### TCP (all failures — zero application payload in entire file)

| Time | Target | Pattern |
|------|--------|---------|
| 15:29–15:30 | **64.227.13.248:80** | SYN **seq=0** → SYN-ACK **+18 ms** → **RST +47 ms** (no ACK, no HTTP) |
| 15:31 | **71.63.243.155:6502** (telnet) | Same as §10r: SYN retry, ACK, **RST ~68 ms** after ACK |
| 21:21–21:22 | **64.227.13.248:80** | SYN; one flow **SYN-ACK only** (no follow-up in capture) |
| 21:22 | **71.63.243.155:6502** | ACK **+12 ms** after SYN → **RST +0.2 ms** after ACK |

**Constant:** client **RST**, TTL **255**, `win=41005`; SYN/ACK use TTL **64**, `win=1460`. **No PSH/HTTP** anywhere (unlike `1006.pcap` ghost GET).

### DNS — replies dropped (Contiki / U2 UDP)

ICMP **port unreachable** from **192.168.0.203** when the router delivers DNS responses:

- **15:29:58** — `192.168.0.1:53` → `192.168.0.203:**1025**` (unreachable)
- **15:31:06 / 15:31:19** — replies to ports **13696**, **13697** (unreachable)

Queries likely went out on those ephemeral ports; by the time the reply arrived, **no lwIP UDP PCB** was bound — consistent with **W5100 socket closed** or **U2 poll too slow** vs Contiki DNS timeout. Explains **ipconfig / browser DNS** pain separate from WAN RTT.

Only explicit DNS query in file: **21:18:57** `0.pool.ntp.org` (port **42473**) — **MegaFlash native NTP** path; **NTP succeeds** (+27 ms).

### ARP / SHAR mismatch (background)

**288** ARP requests: `who-has 192.168.0.203 (00:08:dc:a2:a2:a2)` — default **W5100 SHAR**, not STA **88:a2:9e:48:22:7a**. LAN peers may have stale/wrong L2 mapping for inbound (§1j). Outbound TCP still reaches the Internet.

### Takeaways

1. **Same core failure as §10r/§10s:** ~**50–65 ms** connect budget; **no ACK** (Contiki) or **ACK then immediate RST** (telnet).
2. **DNS is a second bug:** replies arrive but **ICMP unreachable** — U2 UDP socket lifetime / port binding vs reply latency.
3. **§10k-only** re-land should help both if probe-safe; also audit **UDP socket close** on DNS timeout and **SHAR vs STA** for local ARP.

**References:** `sensoroni_onion_1005.pcap`, `1006.pcap`; §10r, §10s; `pico/uthernet2_net.cpp` (`U2_Net_OpenUdp`, `U2_Net_Close`).

---

## 10u. Phase 1 — §10k poll wake only (isolated on §10l)

**Requirement:** Pcap analysis (§10r–§10t) shows SYN-ACK arrives in ~16 ms but **ACK/RST ~47–65 ms** later — core 0 **`U2_Net_Poll`** too infrequent while core 1 runs the II bus. Re-land **§10k only** without §10j recv/TX/RX-reset (§10q/§10p regressions).

**What we did:**

- **`IPCCMD_NET_WAKE`** in **`ipc.h`**
- **`U2_RequestCore0NetPoll()`** in **`uthernet2.c`**: core 1 only, rate-limited **1/ms**, non-blocking FIFO push of static **`u2_net_wake_msg`**
- **`U2_Poll()`** calls **`U2_RequestCore0NetPoll()`** before **`U2_Net_Poll()`** (no-op on core 0)
- **`main.c` `core0Loop`**: **`PicoW_ServiceCore0IpcAndNetwork`** FIFO timeout **50 ms → 5 ms**

**What we did not do:** No changes to **`uthernet2_net.cpp`**, **`u2_tcp_recv`**, **`send_data`**, **`u2_socket_reset_rx`**, or §10m MACRAW gating.

**Validation gate (user):** ip65 detect → Contiki **ipconfig** → telnet pcap: **ACK within ~25 ms** of SYN-ACK, no **RST at +50 ms**.

**References:** `pico/ipc.h`, `pico/main.c`, `pico/uthernet2.c`, `pico/uthernet2.h`; §10k design notes.

---

## 10w. §10m re-land — MACRAW/lwIP ingress split (telnet65 RST fix)

**Symptom:** Local telnet65 to Mac **telnetd** (`192.168.194.172` → `192.168.194.143:2323`): bidirectional pcap shows **SYN-ACK &lt;2 ms**, then **RST from Pico at +12–18 ms** (TTL **255**, win **41005**), then **ip65 ACK ~100–150 ms later**, then server **RST**. UART shows only **sock0 MACRAW** (correct for stock ip65 — no W5100 **`CONNECT`**).

**Why:** **`u2_netif_input_wrapper`** (§**1ar** duplicate feed) copied every inbound frame to ip65 **and** called **`u2_saved_netif_input`**. ip65 builds TCP in software over MACRAW; lwIP has no PCB for that flow → **`tcp_input`** emits **spurious RST** on SYN-ACK before ip65 can ACK.

**What we did (isolated on Phase 1 / §10l + §10k):**

- **`NetworkPump::IsLegacyOperationActive()`** — public; true during **`RunNTP` / `RunTFTP` / `RunTestWifi`**.
- **`u2_netif_input_wrapper`**: if **legacy active** → **lwIP only** (no MACRAW ring copy). Else if **sock0 MACRAW** → **MACRAW copy only**, **`pbuf_free`**, **do not** chain lwIP. Else → lwIP as before.
- **`U2_Net_ServicePoll()`** — MACRAW TX drain on core 0; called from **`NetworkPump::PollOnce()`** so native **`RunX`** inner loops drain deferred MACRAW SEND while legacy owns ingress.
- **`U2_Net_Poll()`** → **`NetworkPump_PollOnce()`** only (single poll entry).

**What we did not do:** No §10j recv/TX/RX-reset; no change to **`send_data`** or **`u2_tcp_recv`**.

**Validation gate:** telnet65 pcap — **no TTL-255 RST** between SYN-ACK and ACK; telnet banner from server; MegaFlash native **NTP** still OK with ip65 MACRAW open.

**References:** `debug/megaflash.pcap` (2026-06-27 16:20); `pico/uthernet2_net.cpp`, `pico/network_pump.{h,cpp}`; §**10m** design; `docs/ip65-Uthernet-II-integration.md` (socket 0 MACRAW only).

---

## 10x. Core 0 poll-before-FIFO + SEND/RECV wake (telnet ACK latency)

**Symptom (post-§10w):** Local telnet65 **sometimes works** (banner, short session) but often stalls: pcap shows **SYN-ACK** and server **banner** with **no ACK** from `.172`; UART shows **`MACRAW rx len=58/91`** then long **`sock0 RECV`** storms, **`MACRAW tx len=1518`**, **`[CYW43] STALL`**.

**Why:** **`PicoW_ServiceCore0IpcAndNetwork`** blocked on **`multicore_fifo_pop_timeout_us(5 ms)`** **before** **`U2_Net_Poll`**, so core 0 could sit in the FIFO wait while inbound frames and deferred MACRAW TX needed service. **`IPCCMD_NET_WAKE`** from **`U2_Poll`** (every **32** U2 bus cycles) helped unblock the wait but did not run **`U2_Net_Poll`** until after the pop returned. During ip65 **RECV-only** polls, **`U2_Poll`** may run less often than **SEND/RECV** commands — ACK/banner responses need core-0 **`U2_Net_ServicePoll`** promptly after **`SN_CR_SEND`**.

**What we did (on §10w + Phase 1 §10k):**

- **`PicoW_ServiceCore0IpcAndNetwork`**: **`U2_Net_Poll` / `U2_MonPollFlush` first**; drain all pending FIFO messages with **0** timeout; optional blocking wait only if **`fifo_timeout_us > 0`**.
- **`core0Loop`** inner wait: FIFO timeout **5 ms → 0** (poll-first loop at full core-0 cadence while Apple connected; user OK with CPU use).
- **`U2_RequestCore0NetPoll()`** on **`SN_CR_SEND`** and **`SN_CR_RECV`** (existing **1/ms** rate limit) so MACRAW TX drain and lwIP service wake sooner than **`U2_Poll`** cadence alone.

**What we didn’t do:** Did not re-land the earlier **500 µs** poll-before-FIFO trial that regressed MegaFlash TFTP (§**7i**) without §**10w** ingress split; did not change **`U2_Net_RecvConfirm`** / §**10j** TCP path.

**Validation gate:** telnet pcap — **ACK &lt;25 ms** after SYN-ACK; interactive session without mid-stream **`STALL`**; native **NTP** still OK.

**References:** `pico/main.c` (`PicoW_ServiceCore0IpcAndNetwork`, `core0Loop`), `pico/uthernet2.c` (`write_socket_register` SEND/RECV), `pico/ipc.h` (`IPCCMD_NET_WAKE`).

---

## 10y. §10x validation — telnet/wacbbs improved, eventual STALL + RECV storm

**Symptom (user, post-§10x):** Performance **much improved**. Local telnet and **wacbbs.ddns.net** both connect and run interactively for a while, then **eventually break**.

**Capture (`debug/megaflash.pcap`, 294 packets):** **Local telnet only** (`192.168.194.172:18147 → 192.168.194.143:2323`). **No RST** from Pico (TTL 255). **SYN-ACK → ACK ~43 ms** (gate was &lt;25 ms — still a large win vs ~100–150 ms pre-§10w). **~11 KiB** payload, **~71 s** session. **Tail:** client **ACK frozen** at **`ack=4173365936`** while server **retransmits** 1-byte PSH (**4173365936:4173365937**) for **20+ s** — ip65 stopped advancing TCP ACK (stuck echo byte).

**UART (`2026-06-27 15-11-46 FT232R USB UART.log`, ~21k lines):** End of session = **`sock0 RECV`** storm (~8 ms spacing, **no SEND**), sparse **`MACRAW rx len=55`**. **wacbbs** traffic **not in pcap** but UART shows same MACRAW patterns (**`rx len=58`** SYN-ACK, **`91`** banner, **`tx len=1518`** bursts). **Failure cluster (~line 764):** burst **`SEND`/`MACRAW tx len=76`**, then **`[CYW43] STALL(0;237-237)`** + **`send_ethernet failed: -2`** (×8), **`RequestAbortAll`** (Apple **/RESET** or abort). **Second boot** at ~line 14058 after another **`RequestAbortAll`** storm.

**Why (emulation gap, not ip65):**

1. **Single-slot MACRAW TX trampoline** (`u2_macraw_tx_pending` + **1518 B** buf): overlapping core-1 **SEND**s can **overwrite** pending frame before **`U2_Net_ServicePoll`** drains; **`send_data`** still **advances `Sn_TX_RD→TX_WR`** unconditionally after **`U2_Net_SendMacraw`** (queue-only path).
2. **`u2_send_macraw_core0`** does not treat **`linkoutput`** failure / **CYW43 STALL** as “not sent” — host believes frame left, wire did not → TCP/ip65 ACK stall, **RECV-only** polling.
3. **§10x** fixed **ingress/poll latency** (handshake, banners); **egress under burst + Wi‑Fi stall** remains the long-session limiter.

**What we didn’t do in this pass:** No **P0-2** MACRAW TX **ring** + **`mq_drop`** counters; no defer-**`TX_RD`** on queue full / **`linkoutput`** fail (§**1ax** honest TX for MACRAW).

**Next lever:** Implement **bounded MACRAW TX queue** (depth **8–16** on RP2350), drain multiple frames per **`U2_Net_ServicePoll`**, **`TX_RD`** advance only on accept/send success; optional **`[u2macraw] tx_q_drop`** telemetry.

**References:** `debug/megaflash.pcap`, `debug/2026-06-27 15-11-46 FT232R USB UART.log`; `pico/uthernet2_net.cpp` (`U2_Net_SendMacraw`, `U2_Net_ServicePoll`), `pico/uthernet2.c` (`send_data` MACRAW **`TX_RD`** advance); §**8** P0-2 / P0-5.

---

## 10z. P0-2 MACRAW TX ring + honest `Sn_TX_RD` (§1ag / §1ax re-land)

**Requirement:** §**10y** validation — long telnet/wacbbs sessions eventually **`[CYW43] STALL`**, stuck TCP ACK, RECV-only storm. Root cause: **1-slot** deferred MACRAW TX overwrite + **`send_data`** advanced **`TX_RD`** when frame only queued or **`linkoutput`** failed.

**What we did:**

- **`U2_MACRAW_TX_Q_DEPTH`**: **16** on RP2350 (**4** on RP2040); **`U2_MACRAW_TX_DRAIN_PER_POLL`** **8**.
- **`U2_Net_SendMacraw`**: returns **0** if accepted, **-1** if queue full or send failed; **`tx_q_drop`** on enqueue reject.
- **`u2_send_macraw_core0`**: returns **`bool`**; checks **`linkoutput`** **`ERR_OK`**; **`lo_err`** / **`pbuf_fail`** counters.
- **`send_data`**: MACRAW path returns **-1** without advancing **`TX_RD`**; **`SN_CR_SEND`** leaves **`Sn_CR=SEND`** set on failure.
- **`U2_TryCompletePendingSocket0Send`**: core-0 retry after drain; called from **`U2_Net_ServicePoll`**.
- Debug UART every **10 s**: **`[u2macraw] tx_q=… tx_q_drop=… lo_err=… pbuf_fail=…`**.

**Validation gate:** Sustained telnet/wacbbs without mid-session ACK freeze; watch **`tx_q_drop`** / **`lo_err`** during stress.

**References:** `pico/uthernet2_net.cpp`, `pico/uthernet2.c`, `pico/uthernet2.h`; §**1ag**, §**1ax**; `docs/Uthernet-II-stack-architecture-and-todos.md` P0-2.

---

## 10za. P0-2 regression — duplicate MACRAW TX (telnet/wget broken, browser OK)

**Symptom (user, post-§10z):** ip65 **telnet** dead (SYN retransmit, no ACK, late Pico RST). Contiki **wget** fails; **browser** still good. UART: duplicate **`MACRAW ptrs`** / **`tx len=42`** per single **`sock0 SEND`**. Pcap: SYN×N, never ACK.

**Why:** **`U2_RequestCore0NetPoll()`** ran **before** **`send_data()`** on **`SN_CR_SEND`**, while **`U2_TryCompletePendingSocket0Send()`** in **`U2_Net_ServicePoll`** could run **`send_data()`** on core 0 while **`Sn_CR` still SEND** — **double (or triple) enqueue/transmit** of the same TX window. TCP handshakes and wget control frames corrupted; browser tolerates looser timing.

**Fix:** Remove **`U2_TryCompletePendingSocket0Send`**; call **`U2_RequestCore0NetPoll()`** **after** **`send_data()`** (and after failed send to drain queue). Keep MACRAW ring + honest **`TX_RD`** on **-1**.

**References:** `debug/megaflash-2.pcap`, UART log tail §P0-2 session; `pico/uthernet2.c`, `pico/uthernet2_net.cpp`.

---

## 10ze. Rollback uncommitted §10zb–§10zd → HEAD §10za (2026-06-27)

**Symptom:** After §10zb (RX reset on OPEN), §10zc (MACRAW RX staging), §10zd (TX reset on OPEN), user report — **telnet65 DHCP/DNS dead** (`tx len=1518` DNS frames, pcap ARP-only); Contiki hardcoded IP but **no DNS**. Stacked experiments regressed bring-up vs §10za validation (browser OK, partial telnet).

**What we did:** **`git restore`** uncommitted changes on **`pico/uthernet2.c`**, **`uthernet2.h`**, **`uthernet2_net.cpp`**, docs → commit **`0a82a91`** (§10za). Rebuilt **`pico2_debug/megaflash.uf2`**. **Did not** revert committed **`2390372`** / **`1b540e9`** / **`0a82a91`** stack unless user requests next.

**Takeaway:** §10zb–§10zd remain design notes only until re-landed **one at a time** with gates (ip65 DHCP → DNS → telnet SYN on pcap).

**References:** SESSION_LOG 2026-06-27 rollback entry; git **`0a82a91`**.

---

## 1ap. tcpdump vs UART: MACRAW ingress length vs host RECV (6502 commit path)

**What:** Correlate **Ethernet framing** with **`tcpdump`** using UART — first at **lwIP → MACRAW enqueue** (historical **`MACRAW rx len=`**, removed), now at **W5100 RECV** after the host has updated **RX_RD** (what the **6502 / ip65 stack committed** as consumed).

**Ingress experiment (May 2026):** **328** Mac→Pico bulk frames in **`debug/tcpdump.txt`** matched **`[u2m] MACRAW rx len=`** in order — framed ingress matched wire lengths (**§1ap** analysis session).

**Current firmware (Debug):**
- **Removed:** **`U2_MonNetRxMacraw`** from **`u2_netif_input_wrapper`** (`uthernet2_net.cpp`) — no log at lwIP ingress.
- **Added:** On **`SN_CR_RECV`** when socket is **MACRAW**, **`u2_mon_emit_macraw_host_consumed`** walks **`old_rd → new_rd`** in the RX ring, parses each **W5100 MACRAW record** (2-byte BE **wire_len** + Ethernet octets), and queues **`U2_MonMacrawHostRx`** (flushed on core 0 as **`[u2m] net sock0 MACRAW host eth_len=… fnv=… seq=… sip→dip`**).
  - **`eth_len`** **== `wire_len − 2`** **==** **`tcpdump`** first-line **`length N`** for that frame (same **Ethernet** byte count as before).
  - **`fnv`** — **FNV‑1a** over the **full Ethernet payload bytes** the ring holds for that frame (offline: compute the same hash from a **`tcpdump -xx`** hex slice or **pcap** extract to compare **byte integrity**).
  - **`sip` / `dip`** — parsed only for **IPv4 + UDP** (otherwise **`0.0.0.0`**); helps line up rows with **`tcpdump`** IP summaries without hashing.

**Caveats:** One **RECV** may commit **multiple** frames; order remains host order. **`fnv`** mismatches implicate **ring / DATA read / RECV** semantics or corruption after enqueue; matching **`fnv`** + **`eth_len`** strongly constrains faults to **software above** the emulated buffer.

**References:** `uthernet2.c` (`u2_mon_emit_macraw_host_consumed`, **`W5100_SN_CR_RECV`**), `u2_monitor.c` (**`U2M_NET_MACHOST_RX`**), `debug/tcpdump.txt`.

---

## 1cf. MACRAW `sock0 RECV` storm (Contiki wget) + telnet $89 — cross-core `Sn_RX_RD` tear & ring full/empty ambiguity

**Symptom:** During a Contiki **wget** download, the UART showed an unbounded **`[u2m] sockN RECV`** storm (thousands of back-to-back RECVs, ~4.46 µs apart) with only the occasional **`net sock0 MACRAW rx len=…`** and, once the ring filled, **`MACRAW rx no-room offered=… accepted=0 free=183 ring=4096`**. Telnet65 failed to connect with **error $89** and streaming sessions had occasional hiccups. The browser worked because it never sustained enough RX to cross the failure window.

**Root cause 1 — cross-core tear on `Sn_RX_RD`.** The 6502/ip65 driver commits the consumer pointer as **two byte stores on core 1**: hi at `Sn_RX_RD0` (`$x28`) then lo at `Sn_RX_RD1` (`$x29`). Core 0 (`u2_push_rx*` free-space math) read those two `u2_memory` bytes back. The previous `read_rx_rd_coherent()` only defended against a stale **high** byte; it did **not** prevent reading **`new_hi:old_lo`** when the pointer crossed a 256-byte boundary (e.g. host going `0x00FE→0x0140`, core 0 samples `0x01FE`). That torn value is *ahead* of the true consumer, so core 0 over-estimated free space and **overwrote unread ring bytes**. The host then read a **corrupted 2-byte MACRAW length header**, advanced `Sn_RX_RD` by a garbage amount, so `Sn_RX_RSR` (derived from `wr−rd`) **never returned to 0** → ip65 kept issuing RECV forever (the storm), and fresh frames eventually hit `no-room`. The same tear intermittently clipped a TCP segment/SYN-ACK, which is why telnet connect returned $89 and streams hiccuped.

**Root cause 2 — ring full/empty ambiguity.** RSR/occupancy is computed from `wr_off − rd_off`. If a push filled the ring exactly (`used == size`, i.e. `wr_off == rd_off`), that state is **indistinguishable from empty**, so a completely full ring reads back as **empty** and the data is silently dropped. This latently corrupts bulk RX independent of the tear.

**Fix (`pico/uthernet2.c`):**
1. Added an **atomic shadow** `uint16_t sn_rx_rd` to `u2_socket_t`. It is published with `__atomic_store_n(..., __ATOMIC_RELEASE)` in `write_socket_register()` **only when the low byte (`Sn_RX_RD1`) is written** — the point at which the host's 16-bit pointer is complete and self-consistent. Core 0 reads it via `u2_rx_rd_load()` (`__ATOMIC_ACQUIRE`) inside `u2_rx_used_bytes()`. `read_rx_rd_coherent()` was removed. The shadow is initialised/kept coherent in `u2_reset`, `u2_reset_socket_rings`, and `u2_socket_discard_rx`.
2. **Reserve one ring byte** in every push path so `used` can never equal `size`: MACRAW too-big guard `total >= size`; all accept checks `free_bytes <= total`; TCP clamps to `usable = free_bytes − 1` and rejects when `free_bytes <= 1`.

**Why the shadow (not a lock or seqlock):** core 1 is the latency-critical bus servicer; a lock/critical-section there risks the RP2350 PIO prefetch Heisenbug (see detection notes). A single aligned 16-bit atomic store on the completing (low) byte gives core 0 an always-complete value with no core-1 stall.

**Build/validation:** Rebuilt `pico2_debug/megaflash.uf2` (RP2350, `./build-debug.sh`). Expectation: `sock0 RECV` converges (no storm) once RSR drains, `no-room` disappears under sustained wget, telnet connects (no $89) and streams without the tear-induced hiccups.

**References:** `pico/uthernet2.c` (`sn_rx_rd`, `u2_rx_rd_load`, `u2_rx_used_bytes`, `write_socket_register` `W5100_SN_RX_RD1` case, `u2_push_rx`/`u2_push_rx_macraw` reserve-byte checks, `u2_socket_discard_rx`, `u2_reset*`); builds on §10l RX pointer geometry.

---

## 1cg. Telnet65 MACRAW TX oversize — `Sn_TX_FSR` advertised full 4 KiB, SEND truncated to 1518

**Symptom (post-§1cf UART, firmware `2026-07-12 01:51:51 UTC`):** Contiki browse/wget no longer RECV-storms (`no-room` absent). Telnet65 still dies after ARP/DNS: MACRAW SEND lines show `ptrs len=1518` with RD/WR spans that decode to **~4 KiB** pending (e.g. `rd=0x024F wr=0x022C` → ~4061 B), then mute empty SENDs and idle RX only.

**Root cause:** Contiki/ip65 issues **one Ethernet frame per SEND** and gates writes on `Sn_TX_FSR >= len`. We reported FSR up to the full **4 KiB** TX ring. A large/corrupt host length (~4K) fills the ring; `send_data()` capped the CYW43 copy at **1518** but still advanced `TX_RD` by the **full** RD→WR span. The host believed the whole buffer left; the wire carried a truncated frame → TCP death. Contiki wget never hit this path (max TX ~313 B in the same capture).

**Fix (`pico/uthernet2.c`):** Cap reported MACRAW `Sn_TX_FSR` at **1518** so the host cannot queue more than one MTU-sized frame into the TX ring. `send_data` MACRAW now logs the **true** RD→WR span on the ptrs line (UART prints `OVERSIZE` when `len>1518`); still MTU-caps the link copy and drains the full window if oversize somehow occurs.

**What we did not do:** Treat Contiki “out of mem” as a Pico RX bug in this capture (no `no-room`; likely 6502 heap / separate from telnet TX). Did not change UDP/TCP FSR (only MACRAW).

**References:** `get_tx_fsr_byte`, `send_data` MACRAW branch; `U2_Net_SendMacraw` already rejects `len>1518`; monitor `U2M_NET_MACTX_PTRS`.

---

## 1ch. RECV storm returned under Contiki — `Sn_RX_RD` publish-on-low-byte was byte-order-dependent (2026-07-12)

**Symptom (UART `Serial Saved Output.txt` + `sensoroni_onion_1017.pcap`, §1cg build):** Browsing worked but was lossy, then "broke" into the classic **`sock0 RECV` storm** (~8900 back-to-back RECVs) with `MACRAW rx no-room … free=36 ring=4096` — `free` **stuck constant**, i.e. the consumer pointer frozen while the host RECV-loops forever. `pcap` corroborated: the HTTP server retransmitted the same 730 B segments many times because the Apple never ACKed the missing region (our RX ring dropped it). Telnet "couldn't get an IP" and later emitted MACRAW SENDs with wild host-written `Sn_TX_WR` (e.g. `wr=0xF012`) = 6502 stack going off the rails after RX never delivered clean DHCP/ARP.

**Root cause — §1cf's fix was byte-order-dependent.** §1cf published the atomic `sn_rx_rd` shadow **on the Sn_RX_RD low-byte write**, on the assumption the driver writes RX_RD **hi (`$x28`) then lo (`$x29`)** — true for ip65's `w5100.s`. **Contiki's W5100 MACRAW driver writes lo-then-hi.** So on the low-byte write we captured `{old_hi : new_lo}` and then **never re-published** when the high byte landed. Whenever RX_RD crossed a 256-byte boundary (e.g. `0x00FE → 0x0140`) the shadow lagged the true consumer by 256 for that step, so `used = wr − rd_shadow` was **over-reported** → `Sn_RX_RSR` never returned to 0 → the host read phantom/stale ring bytes, advanced RX_RD by a garbage length, and RECV-looped forever with `free` pinned small. (Same end state as pre-§1cf, reached from the opposite byte order.)

**Fix (`pico/uthernet2.c`) — publish at the RECV command, not on the byte write.**
1. Removed the `W5100_SN_RX_RD1`-write publish (the byte-order-dependent tear source).
2. In the `Sn_CR = RECV` handler, publish `sn_rx_rd` from the now-complete `u2_memory` `Sn_RX_RD0/1` pair. This is the **hardware-accurate** sync point (a real W5100 only recomputes `Sn_RX_RSR` on RECV), runs on **core 1** (both bytes final, no cross-core read), and is **independent of hi/lo write order**. The published value is always a genuinely committed RD: never *ahead* of reality (so core 0 can't overwrite unread bytes) and never *staled-low across a boundary* (so RSR converges to 0 and the storm cannot form). Between RECVs the producer may use a slightly-stale (never-ahead) RD, which at worst drops an incoming frame — safe, never corrupting.

**What we did not do:** chase the telnet `Sn_TX_WR=0xF012` as a TX tear — `send_data()` runs on core 1 synchronously with the host's TX_WR store, so those are the host's own values (downstream of the RX failure). Left the §1cg MACRAW TX FSR cap and oversize logging in place. Did not treat browsing packet loss as "fix Contiki": the loss is our RX ring dropping bursts while stuck; the fix is convergence, not app changes.

**References:** `pico/uthernet2.c` `write_socket_register` (`W5100_SN_CR_RECV` publish; removed `W5100_SN_RX_RD1` publish), `u2_rx_used_bytes`, `u2_rx_rd_load`, `sn_rx_rd`; supersedes the §1cf publish-on-low-byte scheme (shadow init in `u2_reset*` / `u2_reset_socket_rings` unchanged).

---

## 1ci. §1ch confirmed (storm gone); telnet DNS fails on wild `Sn_TX_WR` — reverted FSR cap + drop desynced TX (2026-07-12)

**Evidence (`Serial Saved Output.txt` + `sensoroni_onion_1019.pcap`, §1ch build):**
- **§1ch works:** 0 `no-room`, only 242 `sock0 RECV` (proportional to 136 `MACRAW rx`) — **no storm**. Contiki browsing healthier (user: "slightly better"). pcap shows real HTTP GET/200 to `66.59.109.26:80` and successful DHCP (`192.168.0.234`). *The banner still read `2026-07-12 01:51:51 UTC` only because `build-debug.sh` rebuilds without reconfiguring, so `FIRMWARE_BUILD_TIMESTAMP` was stale — the behavior proves the §1ch binary was flashed.*
- **Telnet DNS fails:** **zero port-53 packets in the whole pcap** — the DNS query never egresses as a valid frame. Serial telnet session emits repeated `MACRAW ptrs … OVERSIZE` with **wild host `Sn_TX_WR`** (`wr=0xEF2A`, `0xEF18`, `0xEE62`, non-monotonic). `send_data` interpreted `wr<rd` as a full-ring wrap → bogus multi-KB `data_len` → we copied **1518 B of stale ring** to the wire = a corrupt Ethernet frame the DNS server silently drops.

**Two defects addressed:**
1. **Lying about `Sn_TX_FSR` (§1cg).** The MACRAW FSR cap at 1518 was not W5100-accurate. The wild `Sn_TX_WR` high bytes `0xEE/0xEF` line up with `1518 = 0x05EE`, implicating the cap in the telnet host's TX pointer math. **Reverted** `get_tx_fsr_byte` to report true free space.
2. **Emitting garbage on the wire.** On a `data_len > 1518` desync `send_data` now **drops** the frame (logs `MACRAW tx len=0` next to the `OVERSIZE` ptrs line) instead of blasting stale ring bytes, while still retiring the full host TX window so pointers re-sync. Never corrupt the link.

**Still open:** the *root cause* of the telnet host writing non-monotonic `Sn_TX_WR` is not yet pinned (send_data reads TX_WR on core 1 synchronously with the host's store, so it is not a cross-core tear — the host genuinely writes these values). Next capture (FSR uncapped, no wire garbage) will show whether the host's TX pointers normalize once we stop lying about FSR; if not, add host-side `Sn_TX_FSR`/`Sn_TX_RD` read tracing to see the host's decision inputs. Not treating this as "fix telnet65": obligation is W5100-accurate registers.

**References:** `get_tx_fsr_byte` (cap removed), `send_data` MACRAW branch (desync drop), monitor `U2M_NET_MACTX`/`U2M_NET_MACTX_PTRS`.

---

## 1cj. RECV storm root cause: host-facing `Sn_RX_RSR` must be LIVE, not shadow-at-RECV (2026-07-12)

**Symptom (`sensoroni_onion_1020.pcap` + serial, §1ci build):** telnet DNS now resolves (FSR revert worked) and a couple of `GET /` pages complete, but Contiki connections **time out** and launching Contiki re-triggers the `sock0 RECV` storm (`free=36` stuck, then `no-room`). pcap smoking gun: port 1028 sends SYN → receives SYN-ACK → **never sends the ACK**; the server retransmits SYN-ACK with exponential backoff while the Apple spins RECV (~8 µs apart) and the RX ring fills. The storm starts *during* RX, **before** the ring is full — so ring-full is a consequence, not the trigger.

**Root cause — a regression introduced by §1ch.** §1ch (correctly) moved the core-0 `sn_rx_rd` shadow publish to the **RECV command** to kill the byte-order tear. But `Sn_RX_RSR` (the host-facing occupancy register) was *also* computed from that shadow. So between RECV commands `Sn_RX_RSR` was **frozen**. Contiki's W5100 MACRAW driver reads several frames per RECV batch and **re-reads `Sn_RX_RSR` between frame reads**; with RSR frozen high, it kept believing data was available after it had already drained up to `Sn_RX_WR`, so it **read past `wr` into stale ring bytes**, mis-parsed a 2-byte length header, and either advanced `Sn_RX_RD` by garbage or by 0 — freezing the consumer → unbounded RECV storm. One-frame-per-RECV paths (light browsing) happened to keep shadow≈live, which is why it "worked until it broke" under load.

**Fix (`pico/uthernet2.c`) — split the two consumers of `rd`:**
- **Host-facing RSR (core 1):** new `u2_rx_used_bytes_live()` reads the **live** `Sn_RX_RD` straight from `u2_memory`. This is coherent (core 1 both writes `Sn_RX_RD` and reads `Sn_RX_RSR`) and W5100-accurate — RSR shrinks immediately as the host advances `Sn_RX_RD`, so the host never reads past `Sn_RX_WR`. `get_rx_rsr()` now calls it.
- **Producer free-space (core 0):** `u2_push_rx*` keep using the tear-free atomic shadow (`u2_rx_used_bytes()`), which is always ≤ live rd ⇒ conservative ⇒ can never overwrite unread data. §1ch's RECV-time publish and §1cf's init/reset coherence are retained for this path.

**Diagnostic added (§1cj):** `U2_MonRecvStall` (`[u2m] … sock0 RECV STALL rsr=… rd=… wr_off=… hdr=…`) — rate-limited dump when the host issues RECV with `RSR>0` but `Sn_RX_RD` unchanged. Left in the debug build to confirm the storm is gone (should never fire now) and to catch any residual freeze with the exact header bytes.

**Why this is the W5100 model:** on real hardware `Sn_RX_RSR = Sn_RX_WR − Sn_RX_RD` tracks the pointers continuously; writing `Sn_RX_RD` reduces RSR right away. Emulating RSR as a per-RECV snapshot diverged from the chip and is what a slow multi-frame software stack (Contiki) exposed.

**References:** `u2_rx_used_bytes_live` (new), `get_rx_rsr`, `u2_rx_used_bytes` (producer/shadow, unchanged), `write_socket_register` RECV handler (shadow publish + stall diag), `u2_monitor.c/.h` `U2_MonRecvStall`.

---

## 1ck→1cl. RECV wedge self-heal must key on the *header*, not on a frozen `Sn_RX_RD` (2026-07-12)

**Context.** After §1cj (live RSR), a §1ck self-heal was added: if the host issues RECV while `Sn_RX_RD` is *exactly unchanged* (`rd == last_rd`) and the 2-byte length header at `rd` is *impossible* (`framesize == 0` or `> Sn_RX_RSR`), resync `Sn_RX_RD` to the producer `Sn_RX_WR` and let the host recover (TCP retransmits the discarded tail).

**Symptom (`sensoroni_onion_1022.pcap` + serial, §1ck build, banner `20:16:30 UTC`).** Telnet connect + interactive worked cleanly (§1cj/§1ci held). Then a bulk transfer ("send me a ton of data") arrived as ~590-byte frames, the 4 KiB MACRAW ring ran near-full (`no-room offered=590 free=45`, later `free=17`), the host entered a dense RECV burst (~8 µs apart — far too fast to actually read 590-byte frames), and at t≈131 s the **6502 telnet stopped entirely** (no further RECV/SEND). The ring stayed stuck full (`free=17`) while the server retransmitted into a wall every ~3 s (exponential backoff) — the "reconnect swamped by `sock0 RECV`" state. Crucially: **neither `RECV STALL` nor `RECV RESYNC` ever fired.**

**Root cause of the *missed* self-heal.** The §1ck guard required `Sn_RX_RD` to be **exactly frozen** across consecutive RECVs. Under bulk RX the host's `Sn_RX_RD` does not freeze — it **creeps**: it reads a bogus length, advances by the wrong (usually small) amount, and parks on interior frame bytes rather than a boundary. `rd != last_rd` on every RECV ⇒ the freeze counter reset every time ⇒ the self-heal (and the diagnostic) never triggered, and the storm ran unbounded until the mis-reads corrupted/hung the 6502.

**Fix (`pico/uthernet2.c`, RECV handler).** Drive detection off the **header** itself, which is invariant to freeze-vs-creep. A real single-chip W5100 always presents `framesize = frame_len + 2` (≥ 62 for a padded Ethernet frame, always ≤ `Sn_RX_RSR`). We now count **consecutive impossible-header RECVs** (`framesize < 16 || framesize > rsr`, with `rsr > 0`); after 3 in a row we resync `Sn_RX_RD → Sn_RX_WR`, publish the shadow, and emit `RECV RESYNC`. Genuine draining always reads at a frame boundary → header valid → counter stays 0, so the guard cannot fire during normal flow. Removed the `last_rd`/`stall` (frozen-only) state; kept `U2_MonRecvStall` as the onset diagnostic (first hit + every 1024).

**What we did *not* do.** Did not enlarge the MACRAW ring or add TCP-window throttling — the ring is deliberately W5100-sized (4 KiB); bulk overflow + drop + retransmit is correct chip behavior. The defect was that an off-boundary `Sn_RX_RD` wedged permanently instead of self-healing. Did not try to *prevent* the rare bulk-RX boundary desync (root still under investigation — likely large-frame ring-wrap interaction); the header-keyed resync is the robust safety net regardless of how `rd` drifted.

**References:** `pico/uthernet2.c` RECV handler impossible-header block (`bad[]` counter, `U2_MonRecvResync`), `u2_rx_wr_load`, `u2_rx_used_bytes_live`.

---

## 1cm. Traffic corpus (Jul 24–25): checksum/session crashes vs W5100/AppleWin gaps (2026-07-27)

**Corpus:** `/Users/eositis/Documents/AI-work/traffic/` — long UART `2026-07-24 14-51-05 FT232R USB UART #3.log` (firmware **V1.2.2-eo / §1cl** banner `2026-07-12 20:30:24 UTC`), `merged.pcap` / `sensoroni_onion_1024/1025.pcap`, provisional `indexed_traffic.csv` (48% overlap match; most UART MACRAW frames are LAN-local and absent from the server-side pcap).

**What the logs show (not “app checksum bugs”):**
| Signal | Count | Meaning |
|--------|------:|---------|
| `MACRAW rx len=` | 44,338 | Frames accepted into ring |
| `no-room` | 19,646 | Ring full — almost always `free=3` or `28` (W5100-sized full) |
| `RECV STALL` | 1,370 | Host RECV with impossible length header at `Sn_RX_RD` |
| `RECV RESYNC` | 994 | Self-heal discarded unread tail (`rd→wr`) |
| `OVERSIZE` TX | 26 | Host `Sn_TX_WR` desync; we drop (correct) |
| Wire IP checksums in pcap | 0 bad | Corruption is **in the emulated path to the 6502**, not CYW43/WiFi |

STALL headers are mid-frame Ethernet/IP bytes (`C0A8`=192.168…, `4500`=IPv4 hdr, `88A2`/`4822`=Pico MAC fragments, ASCII `312E`="1."), **not** valid MACRAW `wire_len` prefixes — `Sn_RX_RD` is off a frame boundary. ~90% of RESYNCs follow **creeping** `rd` (not a freeze). Each RESYNC throws away up to ~4 KiB of unread ring → Contiki/ip65 see truncated/garbled payloads → **TCP/UDP checksum errors and session death**. `Serial Saved Output.txt` is a later wedged state: continuous `no-room free=3` with the host no longer draining.

**Datasheet / AppleWin gaps still open:**

1. **`Sn_RX_RSR` / `Sn_TX_FSR` byte tear (fixed in §1cm code).** W5100 DS: read **upper byte first, then lower**. Wiznet ioLibrary double-reads until stable. We previously recomputed the full 16-bit value independently on **each** byte access, so a core-0 frame push between RSR0 and RSR1 could return a torn 16-bit size. **Fix:** latch the full value on `*0` (high) read; `*1` returns the latched low byte. Same for `Sn_TX_FSR`.

2. **RSR update policy vs datasheet (still open).** DS: RSR “is automatically changed by **RECV** … and **receiving data**.” AppleWin matches: latched `sn_rx_rsr` increments as RX bytes are written, and `updateRSR()` runs **only on `Sn_CR=RECV`**. Our §1cj host RSR is **live `WR−RD`**, so it also shrinks when the host writes `Sn_RX_RD` *before* RECV — that window is **not** what the datasheet describes. §1cj’s prose claiming continuous shrink is “W5100-accurate” overstates the DS; the live model may still be useful for Contiki, but it is a deliberate divergence from AppleWin/DS and remains a suspect for multi-frame drain races.

3. **AppleWin pulls RX on RSR read; we push async.** AppleWin `readSocketRegister(RSR*)` calls `receiveOnePacket()` then returns a stable latch — single-threaded. We push from core 0 while core 1 serves the bus. Producer uses a RECV-time `sn_rx_rd` shadow (conservative) so we should not overwrite unread bytes; the residual off-boundary `rd` means either (a) the host is still being handed a bad length somehow, or (b) RESYNC fights the host’s software pointer copy after we force `RX_RD=WR`.

4. **`RECV RESYNC` is a safety net that *causes* the checksum symptom.** Discarding the ring tail is not real W5100 behavior. Prefer preventing desync (latch + RSR policy) and consider making RESYNC debug-only or much stricter once the root path is fixed.

5. **TX `OVERSIZE` (26×) and `tx_q_drop`.** Host TX pointer math still occasionally invents ~4 KiB spans; drop-on-oversize avoids wire garbage but the host believes those frames were sent. Root still open (§1ci).

**Code change this pass:** `sn_rx_rsr_latch` / `sn_tx_fsr_latch` in `u2_socket_t`; `read_socket_register` latches on `RSR0`/`FSR0`.

**Next experiments:** rebuild debug UF2 with §1cm latch; retest Contiki/telnet bulk; expect fewer STALL/RESYNC. If desync remains, prototype AppleWin-style RSR (increment on push, recompute only on RECV) behind a flag and A/B against live RSR.

**References:** W5100 DS v1.1.6 Sn_RX_RSR / Sn_TX_FSR; AppleWin `Uthernet2.cpp` `sn_rx_rsr` / `updateRSR` / `getRXDataSizeRegister`; `AI-work/traffic/indexed_traffic_summary.txt`.

---

## 1cn. megaflash-vm / Bramble “flawless U2” is not the Pico `uthernet2.c` path (2026-08-17)

**Question:** Same MegaFlash firmware checksums on real Pico hardware, but Bramble + megaflash-vm have no packet checksum issues.

**What those projects actually do.** megaflash-vm **does not serve `$C0C4–$C0C7` from guest firmware**. Overlay Bramble `host_fast_read`/`host_fast_write` (nibble ≥ 4) call `host_u2_read`/`host_u2_write` in `megaflash-vm/bramble-overlay/host_uthernet.c`. Guest BusLoop inject never updates DATA for U2 (stayed `0x00`). MAME Lua still taps the ports; the W5100 window is **host-completed in the same RPC — AppleWin-style, single-threaded**. Guest `U2_Init`/`U2_Net_Init` may still run for radio/TAP, but the **6502 does not talk to `pico/uthernet2.c`**.

So “same firmware, no checksums in the VM” proves **ip65/Contiki + a correct single-chip W5100 model work**. It does **not** prove `uthernet2.c` + PIO + dual-core are correct. The working model is the overlay, not the Pico U2 bus path.

**What the working host W5100 does that Pico hardware does not (yet):**

1. **No dual-core, no PIO prefetch.** Each `$C0C7` DATA byte is computed and returned in one RPC. Real RP2350 SM1 prefetches the next FIFO byte; if core 1 is late, the 6502 latches a **stale** DATA octet — that is payload/length corruption (checksum errors) that **cannot happen** in the VM. Existing `U2_PeekDataPort` + IRQ0 wait (§1d/§1f) is the hardware analog; load still stresses it.

2. **Do not fill RX while streaming DATA.** `host_u2_read`: skip `host_u2_poll()` when nibble is DATA **and** `u2_addr ≥ $4000`. Comment: `recv()` on every DATA byte tore `Sn_RX_RSR` and wrapped new payload over unread samples (A2Stream sounded out-of-order). TAP sniff returns 0 when MACRAW RX is full so the host TCP window backs up (`[TAP] TCP NAT pause`) instead of ACKing then dropping. On Pico, `busloop` was waking core 0 (`U2_Poll`) every 32 U2 cycles **including** `$C0C7` AI copies, while core 0 CYW43/lwIP keeps pushing MACRAW into the same ring.

3. **RSR/FSR 16-bit pair with lo-first fallback.** Host latches on high byte and sets `u2_rsr_have`; low byte returns the latch and clears the flag; **if low is read first**, it snapshots then. Pico §1cm only latched on high — a low-first read used a stale latch.

4. **No `RECV RESYNC`.** The host never rewrites `Sn_RX_RD` to `WR`. Pico’s self-heal discards tails and is a checksum factory when it fires.

**Code this pass (`pico/`):**
- `sn_rx_rsr_have` / `sn_tx_fsr_have` (port of overlay latch) — **kept**.
- `U2_DataPtrInSocketBuffer()` + skip `U2_Poll` on `$C0C7` AI — **reverted 2026-08-18**. Hardware log after the skip: no STALL/RESYNC, but first ARP TX ~16 s after OPEN; every SEND duplicated (`tx len=` twice); wget SYNs (`len=58`) without timely replies; after a second OPEN the host stopped RECV while RX still arrived. Cause: Pico `U2_Poll` is the §**10k** core-1→core-0 `IPCCMD_NET_WAKE` that **drains TX/lwIP**, not TAP-only fill. VM `host_u2_poll` only sniffs into the ring. Skipping it during DATA AI starved egress. Restore poll every 32 U2 cycles including `$C0C7`.

**Still Pico-only (not copied):** CYW43 has no TAP pause — WiFi ACKs TCP even when the emulated ring is `no-room`. That is drop/retransmit, not checksum-by-itself, unless the ring is also corrupted. PIO prefetch remaining risk under debug UART load.

**Takeaway:** Treat megaflash-vm `host_uthernet.c` as the **behavioral spec** for a working U2 (with AppleWin), not as evidence that guest `uthernet2.c` is already good. Do **not** skip `U2_Poll` during DATA AI on Pico. Hardware work remains: stable 16-bit RSR/FSR, honest `$C0C7` prefetch; ring immutability during AI cannot be done by starving core-0 net.

**References:** `megaflash-vm/bramble-overlay/host_uthernet.c` (`u2_rsr_have`, `host_u2_read` poll skip, `host_u2_on_tap_frame`); `a2bus_bridge.c` `host_fast_read` nibble≥4; `docs/MAME-BRIDGE.md`; Bramble `tapif.c` sniff-return-0; `pico/busloop.c` U2 poll; `pico/uthernet2.c` RSR have-flag.

---

## 1co. `RECV RESYNC` kills wget’s second OPEN (2026-08-18)

**Symptom:** After §1cn poll-skip revert (`11:18:01` banner): first Contiki connect still needs a retry; wget times out. UART: first session eventually does HTTP (`tx 178`, `rx 784`); ~10 s of SYN `len=58` retries before `rx len=58`. Second `OPEN` (wget) then `RECV STALL rsr=44 rd=0x320E hdr=0800` (EtherType, not a MACRAW length) and `RECV RESYNC … rd=0xAD7D -> wr=0x023A`. After that, DNS `tx 83` / `rx 119` then SYN retries with no handshake; host stops RECV; `RequestAbortAll`. Duplicate `MACRAW tx len=` is **log twice** (core-1 enqueue + core-0 `linkoutput`), not two frames on the wire.

**Why RESYNC is wrong here:** After OPEN we zero chip `RX_RD`/`sn_rx_wr`. Occupancy math `wr=0x023A`, `rd=0x320E` ⇒ `rsr=44` matches leftover **host-cached** `Sn_RX_RD` from the previous session (offset `0x20E` still holds previous Ethernet `0x0800`). Forcing `RX_RD=wr` makes the **chip** empty while Contiki still RECV-commits `0xAD7D` — worse than leaving the pointers. A real W5100 never rewrites `RX_RD` on RECV.

**Fix:** Stop rewriting `Sn_RX_RD` on impossible headers (STALL log only). On OPEN, `memset` the socket RX window so a stale cached pointer reads zeros, not an EtherType-as-length. Keep ring pointer reset on OPEN (§1 / Contiki reuse).

**Still open:** First-try SYN-ACK ~10 s late (path/WiFi vs missed frame) without a STALL in that window.

**Status 2026-08-18:** This experiment (no RESYNC rewrite + OPEN `memset`) did **not** restore first-try connect or wget. Reverted in §**1cp**.

---

## 1cp. Revert Bramble/§1cm latch imports; keep pre-import U2 model (2026-08-18)

**What:** After Bramble-inspired ports (§1cm latch, §1cn have-flag + poll-skip, §1co RESYNC-off + OPEN memset), first `asimov.applefritter.com` connect fails until retry and wget fails to connect. **Before those imports** the same stack connected on the first try; the remaining defect was **checksum errors on every 2nd/4th/6th packet**. UART `2026-08-18 12:30:27` (RESYNC rewrite already off): first HTTP after ~10 s of SYN `len=58`; wget second `OPEN` then host stops RECV; ring `no-room free=1`. `tx_q=0` the whole time — the MACRAW queue is empty, not overflowing.

**Why latch broke first connect:** §1cm latched FSR/RSR only on the **high** byte. A host that reads **FSR1 first** (or never hits FSR0 after OPEN) gets `latch=0` → looks like **no TX free space** until a later high-byte read. That matches “first attempt fails, reinitiate works.” The §1cn lo-first `*_have` patch did not restore usability. Poll-skip during `$C0C7` starved §10k drain (reverted earlier). RESYNC-off + memset did not fix first SYN-ACK delay.

**What we did:** Drop `sn_*_latch` / `*_have`. `Sn_RX_RSR` / `Sn_TX_FSR` are again **live per byte** (`get_rx_rsr_byte` / `get_tx_fsr_byte`). Restore §**1cl** RESYNC (`bad>=3` → `RX_RD=wr`). No OPEN RX `memset`. Keep `U2_Poll` every 32 U2 cycles including DATA. MACRAW TX queue unchanged: enqueue on core 1, drain on core 0, pop only after `linkoutput` OK. UART: log `MACRAW tx len=` only from core 0 so SEND is not duplicated in the log.

**What we did not do:** Did not remove the TX queue (needed so core 1 does not call lwIP). Did not skip poll on DATA. Checksum-every-Nth-packet remains the pre-import hardware problem (PIO `$C0C7` prefetch / dual-core RX vs VM host W5100).

**Takeaway:** Do not import megaflash-vm `host_uthernet.c` latch/poll-skip/no-RESYNC onto Pico. VM U2 is single-threaded host-complete; Pico U2 must keep core-0 wake and live FSR for Contiki’s read order.

**References:** `pico/uthernet2.c` `get_tx_fsr_byte` / `get_rx_rsr_byte`; `pico/uthernet2_net.cpp` `u2_macraw_tx_drain`; `pico/busloop.c` `U2_Poll`; §1cl, §1cm–§1co.

---

## 1cq. Latch revert was not the connect/wget bug; `RECV RESYNC` rewrite is (2026-08-18)

**UART** `Firmware build: 2026-08-18 12:53:57 UTC` (§1cp live FSR, §1cl RESYNC rewrite on): same user-visible symptoms.

**First session (browser):** OPEN at 19.5 M ticks; first ARP `tx 42` ~13 s later (host had not SENDed yet — not the TX queue: `tx_q=0`, SEND→wire ~7 ms). DNS `83`/`119` worked. First SYN `58` at 33.8 M, SYN-ACK `rx 58` only at 48.9 M (~15 s TCP RTO). Then HTTP `tx 178` / `rx 784` **succeeded**. Duplicate `tx len=` is gone (core-0-only log).

**wget (second `OPEN`):** OPEN at 64.2 M; ~4 s of RX with **no RECV** (ambient 63/92/243 filling from `wr=0`); then ARP/DNS/SYN. Then `RECV STALL rsr=2625 rd=0x0E95 hdr=2053` and `RECV RESYNC rsr=2172 rd=0x905A -> wr=0x08D6` — chip emptied, Contiki still holding cached `RX_RD`. Further SYNs have no handshake in the rest of the log.

**Why §1cp looked like “no change”:** first-try “failure” is the **15 s SYN RTO** (checksum-class integrity, pre-Bramble). wget death in *this* file is **RESYNC rewriting `Sn_RX_RD`**, which §1cp restored. TX queue is idle (`tx_q_drop=0`).

**What we did:** keep STALL/RESYNC **logs**; **stop writing** `Sn_RX_RD=wr`. No OPEN memset (zeros still look like an impossible length if the host RECV-commits a cached pointer). No FSR latch.

**Open:** 15 s first SYN-ACK — treat as the old every-Nth checksum/integrity problem, not the MACRAW queue.

**References:** `pico/uthernet2.c` RECV `bad[]` block; UART `12:53:57`.

---

## 1cr. `sensoroni_onion_1026.pcap` + UART `13:03:54`: radio vs W5100 ring (2026-08-18)

**UART** banner `2026-08-18 13:03:54 UTC` (RESYNC rewrite off). No `RECV STALL`/`RESYNC`. After wget’s second `OPEN`, host stops `RECV`; ring `no-room free=16`. `tx_q=0` always.

**Pcap** (LAN, Pico STA `88:a2:9e:48:22:7a` → VMware `00:0c:29:2c:bb:f0`, Apple IP `192.168.0.234` → `66.59.109.26:80`):

| Port | Role | What the wire shows |
|------|------|---------------------|
| **1026** | Contiki first TCP (browse) | **58 SYNs, 0 SYN-ACKs.** IPv4/TCP checksums **correct**. `seq=0`, `cksum=0x1d39` every time. This is “first connect fails.” |
| **1027** | Browse retry | SYN (`seq≠0`) + SYN-ACK in ~16 ms, then HTTP. Apple `win=1460`; ACKs lag; server retransmits 730 B segments. Page can complete. |
| **1028** | wget | SYN-ACK OK, GET implied (`ack 139`), server sends HTTP 200 + 730 B (checksums **correct**), Apple **RST**. Same pattern every wget. |

**W5100 vs Pico radio (datasheet MACRAW):**

- Chip: PHY/MAC presents frames into **Sn_RX** until full, then **drops**; host RECV shrinks RSR. No Wi-Fi ACK.
- Pico: `cyw43_arch_poll` → `u2_netif_input_wrapper` **copies** into the 4 KiB ring, **`pbuf_free`, `ERR_OK`**. CYW43 has already **802.11-ACKed**. If `u2_push_rx_macraw` then `no-room`, the peer’s TCP still believes the hop succeeded; the Apple never saw the bytes. VM TAP **returns 0** when the ring is full (pause). That is the gap.
- TX queue is not the failure (`tx_q_drop=0`). SEND copies into a slot then `linkoutput`; 58-byte SYNs appear as Ethernet **length 60** on the wire (padded).

**1026 with valid checksums and no SYN-ACK** is not PIO TX corruption. Same 4-tuple (`192.168.0.234:1026` → `:80`, ISN 0) retried all day. 1027 (different port, ISN ≠ 0) always handshakes. Treat as path/NAT/peer state for that tuple **plus** any SYN-ACK that never shows at this capture. Do not “fix” Contiki ISN; keep looking at whether a reply was eaten on the STA before MACRAW.

**wget RST after good HTTP** is host-side: the server’s payload is checksum-correct on the LAN. Contiki abort/RST matches **corrupt `$C0C7` reads** (PIO prefetch), not a bad radio frame.

**What we did:** `U2_MacrawWifiRxPause()` — when socket 0 is MACRAW and producer free space cannot hold `2+1518+1`, `NetworkPump::PollOnce` **skips** `cyw43_arch_poll` but still **`U2_Net_ServicePoll`** (MACRAW TX drain). Analog of TAP pause. Real W5100 does not pause PHY; this is Pico-radio backpressure so we stop ACK-then-drop.

**References:** `pico/uthernet2.c` `U2_MacrawWifiRxPause`; `pico/network_pump.cpp` `PollOnce`; `u2_netif_input_wrapper`; pcap `sensoroni_onion_1026.pcap`; serial `13:03:54`.

---

## 1cs. WiFi poll-pause did not change connect/wget (`sensoroni_onion_1027`, banner `01:22:15`) (2026-08-18)

**UART:** `Firmware build: 2026-08-19 01:22:15 UTC` (§1cr pause). No `no-room`, no STALL/RESYNC. Same user symptoms.

**Pcap `sensoroni_onion_1027.pcap`:** **`dst port 1026` = 0 packets.** 19 SYNs from `.234:1026` (`seq=0`, cksum OK); **the peer never sends SYN-ACK or any other segment to 1026.** Port **1027** SYN-ACK in 16 ms, HTTP completes. Port **1028** wget: SYN-ACK, GET (`ack 139`), two 730 B HTTP segments (cksum OK), Apple **RST** `ack 731` (~500 ms). UART: first SYN `tx 58` at 58.9 s, first `rx 58` at 70.7 s (that inbound is **1027** SYN-ACK in the pcap, not 1026).

**Why pause could not help:** it only skips `cyw43_arch_poll` when the 4 KiB ring cannot hold one MTU. First-connect failure is **no SYN-ACK on the LAN for 1026** while the ring is nearly empty. wget RST is **after** a valid first MSS on the wire (host TCP accepted `ack 731` then aborted) — PIO/`$C0C7` or 6502 abort, not ring-full.

**What we did:** **Reverted** `U2_MacrawWifiRxPause` / skip-poll. A real W5100 does not pause the PHY; skipping poll can hold later SYN-ACKs in CYW43 if the ring is busy with broadcasts. Keep always-poll + drop-new when full (§1au).

**Next (not this change):** 1026 ISN-0 4-tuple that this Linux/VMware path never answers (same MAC as `.213`); wget payload integrity on `$C0C7` after the first 730 B.

**References:** `sensoroni_onion_1027.pcap`; serial `01:22:15`; §1cr.

---

## 1ct. STA IPv4 masquerade + `$C0C7` peek after IRQ0 (2026-08-18)

**What:** First connect to `asimov.applefritter.com` (Contiki ephemeral **1026**, ISN 0) never gets a SYN-ACK on the Wi-Fi capture; retry (**1027**) works. wget (**1028**) gets a valid first 730 B HTTP MSS then Apple **RST** `ack 731`. Pause-poll (§1cr) did not change this.

**Why (1026):** Pico STA MAC `88:a2:9e:48:22:7a` carries **two** IPv4 addresses: lwIP DHCP **`.213`** and Contiki/ip65 **`.234`**. Ethernet SA is already rewritten to the STA MAC (and ARP SHA). A real Uthernet II has its own MAC, so the W5100 host IP is the only address on that MAC. On this radio, Linux/VMware (sensoroni) never answers **`.234:1026` seq=0** while **`.234:1027`** (nonzero ISN) does — treated as a **shared-MAC / dual-IP** path, not a Contiki ISN bug. VM TAP often NATs; Pico did not.

**Why (wget RST):** Wire checksums on the 730 B segments were good; abort is after the host TCP accepted `ack 731`. Remaining hardware gap is **`$C0C7` PIO prefetch** vs core-0 ring writes. Peeking DATA **before** IRQ0 wait could sample the ring, then wait while core 0 finishes a frame, then push a stale next-byte into SM1.

**What we did:**
- **TX** (`u2_send_macraw_core0`): learn Apple IPv4 from outbound ARP SPA / IPv4 src (not `0.0.0.0`, not STA). SNAT that src (and ARP SPA) to the STA address; recompute IPv4 + TCP/UDP checksums. Skip SNAT for DHCP **68→67**. Restore BOOTP **chaddr** patch (UDP csum 0).
- **RX** (`u2_netif_input_wrapper`): DNAT IPv4 dest STA→Apple and ARP TPA likewise, then push the W5100 ring. Forget the mapping on MACRAW close.
- **`busloop.c`:** wait IRQ0, **then** `U2_PeekDataPort()` into `r[7]`, then `UpdateMegaFlashRegisters(1,…)`.

**What we didn’t do:** Change Contiki ISN; skip `U2_Poll` on `$C0C7`; skip `cyw43_arch_poll`; re-port VM RSR latch.

**Takeaway:** After flash, pcap SYNs should show **src = STA `.213`** (not `.234`) with a valid TCP checksum. First-connect 1026 should then be answerable if the peer was dropping the second IP on that MAC. wget RST: if it persists, the remaining defect is later payload on `$C0C7`, not dual-IP.

**Build:** Pico 2 W Debug UF2 banner **`2026-08-19 01:57:51 UTC`**. Pico W Debug **did not link** (`.heap` overflow **480 B**); NAT helpers are extra code on an already-tight RP2040 Debug image.

---

## 1cu. Banner `01:57:51` / `sensoroni_onion_1028`: NAT missed the wire; wget walked a length header (2026-08-18)

**What:** User: browser **first query worked** (improvement). wget ran for a bit with many checksum errors, then timed out. UART banner **`2026-08-19 01:57:51 UTC`**.

**Pcap (`sensoroni_onion_1028.pcap`, 72 frames, all checksums OK):** Ethernet SA is STA `88:a2:9e:48:22:7a`, but **IPv4 is still Contiki `192.168.0.234`**, not STA `.213`. §1ct SNAT **did not apply**. 1026 SYNs still unanswered; 1027 browse completes with ACK lag; wget **1031/1028** SYN-ACK + GET then **RST ack 731** after the first good 730 B MSS (same as 1027 pcap).

**UART:** No `no-room`. After wget’s second OPEN, many `rx len=784`, then **`RECV STALL hdr=6E2F`** (`n/` from HTML) / `7665` (`ve`) / `2E70` (`.p`) — `Sn_RX_RD` is in the HTTP body, not on a MACRAW length. Then frozen `rd=0xC418` `hdr=0000` and log-only RESYNC (§1cq does not rewrite `RX_RD`). That is the checksum-storm / timeout.

**Why NAT missed:** live `netif_ip4_addr` at `linkoutput` was the only STA-IP source; SA rewrite (no IP needed) ran, IP SNAT did not. Cache STA IPv4 on core-0 `ServicePoll`/`OPEN` and SNAT any non-zero non-STA IPv4 src (still skip DHCP 68→67). One-shot `[u2] sta-nat a.b.c.d -> w.x.y.z`.

**Why wget desyncs:** wire TCP is clean; the 6502 sees a bad 2-byte length and walks the ring. Skip `U2_Poll` on **`$C0C7` reads only** (keep poll on writes so SEND/RECV still wake core 0 — not §1cn). Acquire fence in `U2_PeekDataPort`.

**What we didn’t do:** Re-enable RESYNC rewrite; skip poll on DATA **writes**; change Contiki ISN.

**Build:** Pico 2 W Debug UF2 banner **`2026-08-19 02:27:43 UTC`**. Watch UART for one-shot `[u2] sta-nat 192.168.0.234 -> 192.168.0.213`. Pcap SYNs should then show **src `.213`**.

---

## 1cv. Checksums are `$C0C7`/ring, not dual-IP; revert STA NAT (2026-08-21)

**What:** Re-read `sensoroni_onion_1028.pcap` + UART `01:57:51`. User: Contiki can be a **web server**, so SNAT/DNAT of `.234`→STA `.213` would hide the host address; checksum errors are unlikely to be dual-IP.

**What “checksum error” is:** Contiki/uIP (and ip65 `verifyheader`) recompute the **IPv4 header checksum** and **TCP checksum** over the Ethernet payload copied from W5100 MACRAW (`wire_len − 2` bytes after the 2-byte BE length). The LAN capture of those same frames has **0 bad IP/TCP checksums**. So the 6502 did **not** see the bytes CYW43 delivered.

**Two stages in the UART (same wget):**
1. **Selective fails** — many `MACRAW rx len=784` (= 14+20+20+730) and ACKs, matching “wget worked for a bit.” A few wrong `$C0C7` bytes still look like an Ethernet/IP frame → Contiki prints checksum error and drops that segment. Pcap wget **1031/1028**: Apple’s first ACK after data is still **ack 1** (MSS not accepted), later **RST ack 731** (accepted a retransmit, then aborted).
2. **Stream desync** — `RECV STALL hdr=6E2F` (`n/`), `7665` (`ve`), `2E70` (`.p`) are **HTML body** as a MACRAW length. One bad length (or a skipped/duplicated DATA byte) advances `Sn_RX_RD` into the payload; every later RECV is junk. Log-only RESYNC (§1cq) does not repair `RX_RD` → timeout. This is the same class as §1cm STALL headers (`C0A8`/`4500`/MAC fragments).

**Why “every 2nd/4th/6th” fits hardware, not two IPs:** Socket 0 RX is **4 KiB**; each HTTP MSS is a **786-byte** MACRAW record (`2+784`). Five records ≈ 3930 B; the **6th wraps**. Server also sends **pairs** of 730 B segments in one burst (pcap same timestamp). A stale PIO `$C0C7` byte at the **start of the second frame of a pair** shows up as even-numbered checksum fails, then a wrap turns that into a length-header walk.

**Why dual-IP is the wrong diagnosis:** Checksum is over dest `.234` (what Contiki owns). Changing it to `.213` would not make `$C0C7` match the ring, and would break Contiki **listening** on `.234`. §1ct SNAT **never appeared** on pcap 1028 anyway (SA=STA MAC, IP still `.234`).

**What we did:** **Removed** STA IPv4 SNAT/DNAT (`u2_macraw_sta_nat_*`, STA-IP cache). Kept Ethernet **SA** + ARP **SHA** + DHCP **chaddr** (radio, not host IP). Kept `$C0C7` peek-after-IRQ0, skip `U2_Poll` on DATA **reads**, acquire fence on peek — those are the checksum path.

**Open:** First-connect **1026** with no SYN-ACK remains a **separate** question (not checksum). Next checksum work is PIO/DATA integrity under `mov_data` bursts and 4 KiB wrap, not IP masquerade.

**Build:** Pico 2 W Debug UF2 banner **`2026-08-21 16:17:06 UTC`** (NAT removed).

**References:** pcap `sensoroni_onion_1028.pcap`; AppleWin `writeDataMacRaw` (`size = len+2`, BE); ip65 `w5100.s` `poll` / `ip.s` `verifyheader`; `u2_push_rx_macraw`; §1cm, §10l, §1f.

---

## 1cw. UART `16:17:06`: browse-sized HTTP already walks the length header (2026-08-21)

**Banner:** `Firmware build: 2026-08-21 16:17:06 UTC` (NAT off). One `sock0 OPEN mr=0x44`. `tx_q=0` always.

**Timeline (seconds from boot µs):**
- 20.3 s OPEN. Ambient RX (60/63) until first TX.
- 42.8 s — **before ARP** — `rx 754`, `870`, `757` (unicast to STA MAC; lwIP `.213` traffic can still land in MACRAW).
- 48.4 ARP `tx 42`; 48.9 DNS `tx 83` / `rx 119`; 49.3 SYN `tx 58`.
- 57.1 SYN retry `tx 58` / `rx 58` (handshake); 57.5 GET `tx 178`; **two `rx 784` in 3 ms**, ACK `tx 54`, third `784`, ACK, `rx 424`.
- **62.1 s STALL** `rsr=3454 rd=0x949B rd_off=0x049B hdr=11BC` — 4.6 s after first 784. Then `hdr=4000` (IPv4 DF at IP[6:7]) with `Sn_RX_RD` advancing **+0x4000** per RECV (`rd_off` stuck at `0x0657`). 1043 STALL / 971 RESYNC. Later `hdr=0000` frozen at `rd_off=0x0079`. Four `no-room` once the host stopped draining (`free=70→45`).

**Why this matters:** Desync is **not** wget-only or 4 KiB wrap after many MSS. Three 730 B segments + one 370 B tail is enough. `0x11BC` (4540) is not a legal MACRAW length (`>1518+2` and `>RSR`); Contiki still added it to `RX_RD` (`0x949B+0x11BC=0xA657`). Skip `U2_Poll` on `$C0C7` reads did not stop it. TX path still ran (14 SENDs).

**Takeaway:** Next fix is the **first bad 2-byte length** under a short `mov_data` burst (PIO prefetch / DATA coherence), not NAT and not ring-full.

**Second capture (same banner):** First browse **completed**. `754/870/757` arrived **after** that HTTP (not before ARP) — pre-ARP STA unicast is **not** required. wget then second `OPEN`; ~20× `784` (~4× 4 KiB wrap); STALL `hdr=5370` (`Sp`) and later `88A2` (STA MAC as length). Added first-STALL **`dbg e66cf7 A/C`**: `match=1` means last producer record header still equals `last_wire` (ring write OK → host `$C0C7`/`RX_RD`); `match=0` means that header is already wrong (core-0 ring).

**Third capture (`20:21:09`):** wget “did not start” = no STALL. Browse worked (GET `178`). wget GET `192` + two `784` then new `OPEN`; later SYNs had **no** matching `rx 58` (SYN-ACK never enqueued). A/C dump **inconclusive**. Hyp **H**: MF filter drops TCP unicast if dest≠SHAR — log `mf-filter` / `dbg e66cf7 H`.

**UART `12:33:25` DHCP storm:** `match=1` (`last_wire=65` == `hdr_at_rec=0x0041`). Producer ring OK; 6502 `RX_RD` mid-payload (`hdr=4F13`, `ring8=4F13…0806`). Hyp **C rejected**, **A/B confirmed**. No `tx` before STALL — DHCP discover never left. A/C/STALL/RESYNC UART once per OPEN.

**pcap `sensoroni_onion_1033` (2026-08-22):** 189 frames, 48 min. Wire IP/TCP checksums all good; Ethernet SA = STA MAC; IPv4 = Contiki `.234` (no `.213`). **`:1026` SYN `seq=0` never SYN-ACKed** (52 SYNs). **`:1028`/`:1030` wget** handshake + two 730 B HTTP then Apple **RST**. Browse `:1027` completes with delayed ACKs/retransmits. Confirms: unanswered first-connect is **on the LAN** (not MF dropping SYN-ACK); wget RST is **host `$C0C7`**, not bad radio L4.

**References:** UART `Serial Saved Output.txt` (`16:17:06`); §1cv.

---

## 1cx. The periodic corruption source is `u2_poll_counter` in `BusLoop()`, not the ring (2026-08-29)

**What:** User report — inbound frames are corrupted with a bad checksum on roughly **every 3rd or 6th packet**, after the radio has already ACKed, so no resend can be requested. §1cu/§1cv/§1cw had localised this to "`$C0C7`/ring, not the wire" and to "the first bad 2-byte length under a short `mov_data` burst", but no periodic trigger had been identified.

**Evidence that the producer is innocent:**
- `debug/2026-08-18 07-11-25 FT232R USB UART.log`: the `dbg e66cf7 A/C` self-check is **`match=1` in 1092 of 1092** samples. The 2-byte length header we wrote is exactly where we wrote it, with the right value. `u2_push_rx_macraw` and the ring wrap are correct.
- The host is behaving *correctly* given what it reads. Two independent STALL/RESYNC pairs confirm `rd_new = rd_old + hdr` exactly: `0x554E + 0x7665 = 0xCBB3` and `0x57B3 + 0x6C65 = 0xC418`. Contiki faithfully reads a 2-byte length and advances `Sn_RX_RD` by it; the headers it reads are ASCII HTTP body (`"ve"`, `"le"`, `"n/"`). So the ring content is right and the *delivery* of bytes over `$C0C7` is wrong.
- Wire captures (§1cw, `sensoroni_onion_1033`) show all IP/TCP checksums good. Corruption is introduced between `u2_memory` and the 6502.

**Root cause — the poll gate fires in the inter-frame register dance.** `pico/busloop.c`:

```c
if (!(addr == U2_C0X_LAST && (busdata & READFLAG))) {
  if (++u2_poll_counter >= 32) { u2_poll_counter = 0; U2_Poll(); }
}
```

§1cu excluded `$C0C7` **reads** from incrementing the counter. That protects the cycle the poll runs on, but **not the cycle after it** — and it has a side effect nobody accounted for: during a `mov_data` burst the counter does not advance at all, so `U2_Poll()` can *only* fire during the register dance between frames (address writes, `Sn_RX_RD` update, `Sn_CR`=RECV). That is precisely the sequence that ends in the first `$C0C7` read of the **next frame's length header**.

Per MACRAW frame Contiki issues roughly 13 counted (non-`$C0C7`-read) cycles: 2 to point at `Sn_RX_RSR`, 2 for `Sn_RX_RD`, 2 to set the data address, 4 to write `Sn_RX_RD` back, 3 for `Sn_CR`. `32 / 13 ≈ 2.5`; a leaner path (~5 counted cycles) gives `32 / 5.3 ≈ 6`. **That is the reported "every 3rd, or 6th packet" exactly.**

**Why a delay corrupts data at all.** `a2bus_rp2350.pio` serves 6502 reads with **no CPU involvement**: `mov osr, rxfifo[y]` / `out pins, 8`. The byte the 6502 latches is whatever core 1 last wrote into `pio0->rxf_putget[SM_A2BUS][1]`. Core 1 is always one cycle behind and must have `registers.r[7] = U2_PeekDataPort()` pushed before the next read reaches `mov osr, rxfifo[y]` — about **90 ns after nDEVSEL falls** (`irq set 0 [3]` … `[1]` ≈ 13 PIO cycles at 150 MHz). Two failure modes, both giving the same result:
- Core 1 has not pushed chunk 1 yet → the 6502 latches the **previous** byte.
- `a2buslistener` uses `push noblock` into a 4-deep FIFO with **no overflow detection anywhere in the tree** → the cycle is silently discarded, `u2_data_address` never advances, and the next read returns the same byte.

Either way one byte is **duplicated and the rest of the frame shifts by one** — mostly-correct data that fails checksum, and a mis-read length header when it lands on one. That is the observed signature.

**Why `U2_Poll()` is expensive enough to matter.** `U2_Poll` and `U2_Net_Poll` are **not** in `U2_BUS_RAM`; they execute from XIP flash while core 0 is running lwIP/cyw43 from the same XIP cache. `U2_RequestCore0NetPoll` additionally does `get_absolute_time()` (APB timer), `absolute_time_diff_us`, then `multicore_fifo_push_timeout_us` (which itself calls `make_timeout_time_us` and `time_reached`). Nominal cost is a few µs against a ~15 µs budget, but under XIP contention it is unbounded — which is why the fault is intermittent rather than every time.

**Fix (landed 2026-08-29):**
1. **Deleted the `u2_poll_counter` block from `BusLoop()`** (`pico/busloop.c`). Core 1 now does nothing on a `$C0C4–$C0C7` cycle beyond servicing it. Safe because `U2_RequestCore0NetPoll()` still fires from the SEND and RECV handlers, and `PicoW_ServiceCore0IpcAndNetwork(0)` polls the network unconditionally with a zero FIFO timeout. `U2_Poll()` had exactly one caller and was removed with it.
2. **`U2_RequestCore0NetPoll()` is now a single SRAM store** to `volatile bool u2_core0_net_wake_pending`, cleared by `U2_Net_Poll()` on core 0. Dropped the `get_absolute_time()` / `absolute_time_diff_us` / `multicore_fifo_push_timeout_us` sequence and the 1 ms rate-limit entirely — there is no blocking wait to unblock.
3. **Moved the bus-path register plumbing into SRAM** via the existing `U2_BUS_RAM` macro (RP2350 only; no-op on RP2040 so its RAM budget is untouched): `read_socket_register`, `write_socket_register`, `get_rx_rsr`, `get_rx_rsr_byte`, `get_tx_fsr`, `get_tx_fsr_byte`, `get_tx_data_size`, `u2_rx_used_bytes_live`, `read_net16`, `U2_RequestCore0NetPoll`. Verified with `nm`: all now at `0x2000xxxx` on RP2350 and still at `0x10xxxxxx` on RP2040. This supersedes the §1d note that `read_socket_register` "stays in flash".

**Still to do:** `FDEBUG.RXSTALL` drop detection. RP2350 PIO sets that bit for a SM whose `push noblock` discarded a word; counting it during a wget would convert the dropped-cycle half of the mechanism from inference to proof, and would catch any regression. Also worth tracking the listener FIFO high-water mark.

**What we are not doing:** re-enabling the §1cq RESYNC rewrite, touching the ring/producer (proved correct by `match=1`), or revisiting NAT (§1cv already reverted it).

**Secondary divergence found, not the cause:** `read_socket_register` recomputes `Sn_RX_RSR`/`Sn_TX_FSR` on **each byte** access, so a 16-bit read is torn (hi at T1, lo at T2 with core 0 free to advance `sn_rx_wr` between). A real W5100 latches the pair. Analysis shows this can only **under**-report (`wr` is monotonic between the two reads), so it costs an extra poll rather than corrupting data — worth fixing for fidelity, but it does not explain the checksum failures.

**References:** `pico/busloop.c` (`u2_poll_counter`), `pico/a2bus_rp2350.pio` (`irq set 0` / `mov osr,rxfifo[y]`, `a2buslistener` `push noblock`), `pico/a2bus.h` (`UpdateMegaFlashRegisters`), `pico/uthernet2.c` (`U2_Poll`, `U2_RequestCore0NetPoll`, `U2_PeekDataPort`, `read_socket_register`), `debug/2026-08-18 07-11-25 FT232R USB UART.log`; §1cu, §1cv, §1cw.

---

## 1cy. §1cx result: stalls cured, checksums unchanged → bus-path instrumentation (2026-09-01)

**User result after the §1cx build:**
- **`RECV STALL` / `RECV RESYNC` gone.** Downloads now run far longer before failing (Contiki wget eventually dies).
- **Checksum errors persist, unchanged in rate.**
- **adtproeth** (ADTPro over Ethernet, UDP path) works briefly then crashes out.
- **a2stream** cannot connect at all — likely the hardware **TCP socket** path, not MACRAW.

**What this tells us.** §1cx was a real defect and its removal fixed exactly what it should have: the poll gate was firing in the inter-frame register dance, mis-delivering the length header and derailing `Sn_RX_RD` into the wedge. With core-1 latency on that path now minimal, the derailment is gone — so the *periodicity* of the wedge was the poll gate. But the underlying **byte corruption is a separate defect** and is not caused by core-1 work on the bus path, because there is no longer any.

**What is now eliminated.**
- **The RX ring / producer.** `match=1` on 1092/1092 producer self-checks (§1cx), plus a static argument: the host only ever reads inside `[rd, wr)`, and the producer's free-space math uses the shadow `sn_rx_rd`, which is published at RECV and therefore always at-or-behind the live `Sn_RX_RD`. So the producer can never overwrite a byte the host is about to read. The ring cannot be the corruption source.
- **Core-1 latency from the poll gate** (§1cx, now removed).
- **The wire.** All IP/TCP checksums good in `sensoroni_onion_1033` (§1cw).

**Remaining hypothesis space, and why it is small.** Corruption must therefore be in the *delivery* of bytes over `$C0C7`:
- **H1 — dropped bus cycle.** `a2buslistener` uses `push noblock` into a 4-deep FIFO. On overflow the record is silently discarded, `u2_data_address` never advances, and the next read returns the same byte. **There is no overflow detection anywhere in the tree.**
- **H2 — prefetch turnaround.** The a2bus SM latches chunk 1 about 140 ns after nDEVSEL falls (`irq set 0 [3]` … `mov osr,rxfifo[y]`), so core 1 must have pushed the *next* byte before the *next* cycle starts. This is a hard per-cycle deadline that no amount of general speed-up removes; it is structural to the one-deep `rxf_putget` latch.
- **H3 — ring geometry disagreement.** Our `auto_increment` wraps at `0x6000`/`0x8000` (the W5100 TX/RX *memory* blocks), but socket 0's buffer is only 4 KiB (`ring=4096` in the logs). If the host does not rewrite the address register at `receive_base + receive_size`, it reads `0x7000+` — socket 1's area — and every frame crossing the socket-buffer end is corrupt. `4096 / 1516 ≈ 2.7`, which independently matches the reported "every 3rd" beat.

**Instrumentation added (Debug build only).** One run of a wget now discriminates all three:

| Counter | Meaning | Verdict if non-zero |
| --- | --- | --- |
| `[busdiag] behind` | listener FIFO was already non-empty when core 1 popped a `$C0C7` read | **H2** — the queued cycle was answered from a prefetch computed one cycle early, so the 6502 got a duplicate byte. This is a *direct* count of corrupted bytes, not a proxy. |
| `[busdiag] rxstall` | `FDEBUG.RXSTALL` for `SM_LISTENER` (write-1-to-clear) | **H1** — a cycle was silently discarded |
| `[busdiag] fifo_hw` | high-water listener FIFO level | how deep the backlog gets |
| `[u2diag] data_oow` | `$C0C7` reads inside RX memory but outside socket 0's `[receive_base, +receive_size)` | **H3** — host walked past its socket buffer without rewriting the address register |
| `[u2diag] rx_geom` | per-socket base+size, plus RMSR/TMSR/MR/ptr | shows whether our geometry matches what the driver asked for |

Why `behind` is a valid direct measure: the listener pushes ~70 ns after nDEVSEL, and the a2bus SM latches chunk 1 at ~140 ns. So if a cycle is already queued when core 1 pops the previous one, the queued cycle has necessarily already been answered from the stale latch. In healthy operation this must be **0** — even the tightest 6502 sequence (`eor $C0C7` back-to-back, the §1d RTR probe) leaves ~4 µs between accesses.

**Cost / caveat:** one APB read (`pio0->flevel`) plus one `pio0->fdebug` test per bus cycle, Debug only, ~20–40 ns against a ~12 µs budget. Release is byte-identical to §1cx (verified: `nm` finds no `BusDiagReport` / `U2_DiagReport` / `g_bus_behind` in `pico2_release`).

**If H2 is confirmed, the fix is structural, not a speed-up.** The data port must stop depending on per-cycle core-1 turnaround: replace the one-deep `rxf_putget` chunk-1 latch for `$C0C7` with a real PIO **TX FIFO** that core 1 keeps stuffed several bytes ahead, so the SM pops the next RX byte itself on each read. That converts a hard 140 ns deadline into a 4–8 byte pipeline. It is a `a2bus_rp2350.pio` rewrite and should not be attempted before the counters say it is needed.

**Separate defects, not covered by the above:** `a2stream` failing to connect exercises the hardware TCP socket path (`U2_Net_OpenTcp` / `U2_Net_ConnectTcpEx` / `tcp_bind` on `Sn_PORT`), which shares nothing with the MACRAW ring but the `$C0C7` transport; adtproeth is the UDP path (`u2_push_rx` with the 4+2+2 header). Both need their own `[u2m]` OPEN/CONNECT traces before diagnosis.

**References:** `pico/busloop.c` / `pico/busloop.h` (`g_bus_behind`, `g_bus_rxstall`, `BusDiagReport`), `pico/uthernet2.c` / `pico/uthernet2.h` (`g_u2_data_oow`, `U2_DiagReport`), `pico/uthernet2_net.cpp` (report call in the 10 s `U2_Net_ServicePoll` block), `pico/a2bus_rp2350.pio`; §1cx.

---

## 1cz. The §1cy Debug build was an invalid test vehicle — instrumentation moved to Release (`MF_BUS_DIAG`) (2026-09-01)

**What.** §1cy shipped the new counters as `#ifndef NDEBUG` and the instruction was to flash `pico2_debug/megaflash.uf2`. The result was a regression against the §1cx Release build the previous data point came from: Contiki struggled to connect and download, wget did not connect at all, and a2stream could not create a connection. No `[busdiag]` data was obtained.

**Why this was my methodology error, not new information about the bug.** Three independent defects in the test setup, in descending order of severity:

1. **The Debug build starves core 0's lwIP poll through blocking UART.** `PicoW_ServiceCore0IpcAndNetwork` calls `NetworkPump_PollOnce()` and then `U2_MonPollFlush()` on every iteration. The flush formats up to `U2_MON_FLUSH_MAX` = **128** events per call with `printf` to a **115200 baud** UART, and `printf` blocks when the TX FIFO is full. 128 lines × ~80 chars ≈ 10 KB ≈ **890 ms of blocking output per iteration**. (§1de: this section originally said 48/330 ms — the constant in `u2_monitor.c` is 128, so the effect is ~2.7× worse than first recorded.) Under any real traffic the monitor produces events far faster than 115200 can drain, so core 0 lives inside `uart_write_blocking` and `NetworkPump_PollOnce` runs a handful of times per second instead of thousands. TCP handshakes need core 0 to answer SYN/SYN-ACK and service cyw43 — hence **"cannot connect"**, in all three applications at once. This is a property of the Debug build, unrelated to §1cx or §1cy.
2. **Debug is `-Og`, Release is `-O3`** (confirmed from `flags.make`, not assumed). §1cx established a hard ~140 ns chunk-1 deadline on the bus path; a whole optimization tier of extra instructions in `BusLoop` and `U2_HandleBusAccess` is material against that budget, so Debug cannot be used to measure bus timing either.
3. **The counters were sampled on the wrong side of the deadline.** `pio_sm_get_rx_fifo_level` and the `pio0->fdebug` test were placed *before* `U2_HandleBusAccess` and therefore before `UpdateMegaFlashRegisters(1, …)`. They delayed the very prefetch publish whose lateness they were counting: a textbook observer effect that inflates `behind` and can manufacture corruption. The §1cy claim that the cost was "~20–40 ns against a ~12 µs budget" was **wrong** — the relevant budget is the ~140 ns to chunk-1 latch, not the ~12 µs inter-cycle gap.

**What we did.**

- Added CMake cache var **`MF_BUS_DIAG`** (default `0`), deliberately decoupled from `CMAKE_BUILD_TYPE`, and switched every counter guard from `#ifndef NDEBUG` to `#if MF_BUS_DIAG`. The 10 s stats block in `U2_Net_ServicePoll` is now `#if !defined(NDEBUG) || MF_BUS_DIAG` so the report fires in Release. New build tree `pico2_diag` = **Release + `-O3` + `NDEBUG` + no `[u2m]` monitor + counters**, which is byte-for-byte the §1cx timing environment plus a 10 s printf.
- **Moved the FIFO sample after `UpdateMegaFlashRegisters(1, …)`.** The measurement stays valid and is arguably stricter: a cycle still queued *after* the prefetch is published was unambiguously answered from the previous chunk-1 value.
- **Removed the `pio0->fdebug` access from the bus path entirely.** RXSTALL is a sticky write-1-to-clear flag, so core 0 harvests and clears it inside `BusDiagReport()`. The bus path now costs one APB read on `$C0C7` reads only, after the deadline.

**What we did not do.** We did not change any emulation logic in response to the reported regression, and we are not treating "wget did not connect" as a new W5100-emulation symptom — there is no evidence for that yet, because the build that produced it could not have connected regardless.

**Takeaway, beyond this fix.** The Debug build cannot be used to judge connection-level behaviour, and much of the historical UART evidence was captured on it. Symptoms recorded in §10r ("slow core-0 lwIP poll"), §10t (SYN→RST at ~47–62 ms, DNS reply → ICMP port unreachable) are exactly what a core 0 pinned in blocking UART output looks like. **Some previously chased "emulation bugs" may be Debug-harness artifacts and should be re-confirmed on `pico2_diag` before further work.** If the monitor is needed alongside connectivity, the UART must be raised well above 115200 (§1aw already used 460800) or `U2_MON_FLUSH_MAX` cut hard.

**References:** `pico/CMakeLists.txt` (`MF_BUS_DIAG`), `pico/busloop.c` / `pico/busloop.h` (sample moved after the chunk-1 publish; RXSTALL harvest in `BusDiagReport`), `pico/uthernet2.c` / `pico/uthernet2.h`, `pico/uthernet2_net.cpp`, `pico/u2_monitor.c` (`U2_MonPollFlush`, `U2_MON_FLUSH_MAX`), `pico/main.c` (`PicoW_ServiceCore0IpcAndNetwork`); §1cx, §1cy, §10r, §10t.

---

## 1da. `pico2_diag` was unreadable: Release logs to USB only, and USB halts the bus loop (2026-09-01)

**What.** The §1cz `pico2_diag` image (Release + `MF_BUS_DIAG=1`) could not be used at all. Its output went out **USB CDC**, and connecting USB kills storage — so there was no way to read the counters while the thing being measured was running.

**Why — two independent facts about Release builds that §1cz did not account for.**

1. **Release disables the UART stdio driver outright.** `main.c` had `#ifndef NDEBUG → stdio_uart_init()` / `#else → stdio_set_driver_enabled(&stdio_uart, false)`. Every Release build therefore has exactly one stdio sink: USB CDC (`pico_enable_stdio_usb(… 1)` is unconditional in `CMakeLists.txt`). All of `[u2macraw]` / `[busdiag]` / `[u2diag]` went to USB.
2. **In Release, USB and the Apple bus are mutually exclusive *by design*.** `ReleaseUpdateBusUsbGate()` sets `g_release_bus_emulation_enabled = IsAppleConnected() && !stdio_usb_connected()`, and `GetAppleBusBlocking()` in `a2bus.h` reacts to it by spinning **without draining the listener FIFO**:

```c
while (!g_release_bus_emulation_enabled) { tight_loop_contents(); }
```

So plugging USB stops core 1 servicing bus cycles entirely — not just storage, but the U2 emulation too. This is deliberate (USB console vs bus mode), not a defect. Combined with (1) it makes a Release diagnostic build self-defeating: reading the log requires the exact condition that disables what the log measures. The user's report — "if USB is connected, storage does not work" — is this gate, and the two remedies they identified (log to UART, or allow USB and storage together) are the only two options.

**What we did — the UART option.** For `MF_BUS_DIAG` builds only:

- `main.c`: the UART-disable is now `#if defined(NDEBUG) && !MF_BUS_DIAG`, so a diag build calls `stdio_uart_init()` + `setbuf(stdout, NULL)` exactly like Debug. UART 115200; the diag output is three lines every 10 s, so baud is not a constraint here (contrast §1cz, where the Debug `[u2m]` firehose saturated the same UART).
- `main.c`: both `stdio_usb_init()` calls are `#if !MF_BUS_DIAG`. Confirmed with `nm` that the linker drops `stdio_usb_init` from `pico2_diag` entirely — USB is power-only, and TinyUSB never enumerates, so there is also no 1 ms `tud_task` timer or USB IRQ adding jitter to the measurement.
- `misc.c`: the gate reads a new `release_usb_console_active()` which returns `false` under `MF_BUS_DIAG`. This keeps `g_release_bus_emulation_enabled = IsAppleConnected()`, so **a USB cable plugged in for power no longer halts the bus loop**, and it avoids querying TinyUSB state that was never initialized.

**What we did not do — the "USB and storage together" option.** Relaxing the gate for normal Release builds was rejected for now: the gate is load-bearing and its removal would put USB IRQs, a 1 ms `tud_task` alarm, and a potentially blocking `stdio_usb_out_chars` (up to `PICO_STDIO_USB_STDOUT_TIMEOUT_US`) alongside the ~140 ns chunk-1 deadline from §1cx. Doing that *while* trying to measure that deadline would confound the experiment. It stays a separate piece of work.

**Takeaway.** Any future Release-based diagnostic build must log over UART. Release has no other usable sink, and reaching for USB switches the firmware into a mode where the bus loop is stopped by design.

**References:** `pico/main.c` (UART init guard, both `stdio_usb_init` guards), `pico/misc.c` / `pico/misc.h` (`release_usb_console_active`, `ReleaseUpdateBusUsbGate`, `g_release_bus_emulation_enabled`), `pico/a2bus.h` (`GetAppleBusBlocking` gate spin), `pico/CMakeLists.txt` (`MF_BUS_DIAG`, `pico_enable_stdio_usb`); §1cx, §1cz.

---

## 1db. First `MF_BUS_DIAG` capture: H2 (prefetch turnaround) is dead; the failure is upstream of the bus (2026-09-01)

**Capture:** `Serial Saved Output.txt`, 24 report windows (~4 min), Pico 2 W, `pico2_diag`. Session: a web page loaded with issues, downloads did not work, wget did not connect.

### Result 1 — the bus delivery path is clean. H2 is eliminated.

```
[busdiag] c0c7_reads=38807 behind=0 (0 ppm) fifo_hw=0 rxstall=7
```

**`behind=0` over 38,807 `$C0C7` reads, and `fifo_hw=0`** — the listener FIFO was *never* observed non-empty after core 1 published chunk 1. Core 1 always wins the ~140 ns turnaround. Combined with §1cx (poll gate removed) this **closes H2**, and with it the proposed `a2bus_rp2350.pio` TX-FIFO rewrite: **do not build it.** That was the most expensive item on the list and it is now off it.

`data_oow` reached **4** and then froze — 4 reads in the 8 KiB RX region outside socket 0's `0x6000+4096` window, all in two adjacent windows, then never again. Real but tiny, and it stopped before the session wedged, so it is not the mechanism behind a sustained failure. H3 is not the main event.

Geometry itself is **correct**: `RMSR=0x06` → s0 4 KiB @ `0x6000`, s1 2 KiB @ `0x7000`, s2/s3 1 KiB @ `0x7800`/`0x7C00`, totalling exactly 8 KiB. `MR=0x03` (indirect + auto-increment). ip65 init completed normally.

### Result 2 — `rxstall` was mis-measured (my bug)

RXSTALL is **sticky**, and §1cz harvested it *once per 10 s report*. So `g_bus_rxstall` could only ever increment by 1 per window: `rxstall=7` means "7 of 24 windows contained at least one dropped cycle", not "7 dropped cycles" — it cannot distinguish one drop from millions. Also, **5 of the 7 occurred while `c0c7_reads` was still 0**, i.e. before any U2 traffic; those are consistent with the Release bus gate (§1da) spinning in `GetAppleBusBlocking()` *without draining the FIFO*, which necessarily overflows it and sets RXSTALL. Fixed: the flag is now checked and cleared **per bus cycle**, still after the prefetch publish so it costs no deadline.

### Result 3 — the counters could not see the actual failure

The revealing line is the steady state at the end:

```
[busdiag] c0c7_reads=38807 …          (~6,000 reads per 10 s window, sustained)
[u2diag]  … MR=0x03 ptr=0x0428
[u2macraw] tx_q=0 tx_q_drop=0 lo_err=0 pbuf_fail=0
```

`ptr` is `u2_data_address`, and **`0x0428` is socket 0's `Sn_RX_RD`** (`S0_BASE 0x0400 + 0x28`). It sits there, unchanged, for the last six windows (60 s) while the host issues ~600 `$C0C7` reads per second. That is a **6502 poll loop spinning on the receive pointer for data that never comes** — entirely consistent with "wget did not connect at all". Note the host is *not* reading frame data: a draining host would show `ptr` walking through `0x6000–0x6FFF`.

So the defect is **upstream of the bus transport**, in whether frames ever reach the ring — and §1cy/§1cz instrumented only the delivery path. The `[u2macraw]` counters are TX-queue-only (`tx_q`, `tx_q_drop`, `lo_err`, `pbuf_fail`) with no success counter, so all-zero there is uninformative rather than reassuring.

**What we added** (all `MF_BUS_DIAG`, in `U2_DiagReport`): `rx_push` (frames written to the ring), `rx_drop` (ring full / frame too big), `rx_filt` (rejected by the `Sn_MR` MAC filter — §1az added this filter, and an over-tight filter would produce exactly this silence), `recv` / `send` (`Sn_CR` commands from the 6502), plus a per-socket line with `Sn_MR`, `Sn_SR`, buffer geometry, `Sn_RX_RD`, `sn_rx_wr` and live `Sn_RX_RSR`. One capture now separates: nothing arriving from Wi-Fi (`rx_push=0`), arriving but filtered (`rx_filt` climbing), arriving but discarded (`rx_drop`), or arriving and ignored by the host (`rx_push` climbing with `rsr` growing and `recv` flat).

**Takeaway.** §1cy framed this as three hypotheses all located on the bus path. The data says the bus path is fine and the question was scoped too narrowly. `behind=0` is a solid, reusable result; the rest of the investigation should move to the RX ingest path.

**References:** `Serial Saved Output.txt`; `pico/busloop.c` (per-cycle RXSTALL), `pico/uthernet2.c` (`g_u2_rx_push` / `g_u2_rx_drop` / `g_u2_rx_filtered` / `g_u2_recv_cmd` / `g_u2_send_cmd`, expanded `U2_DiagReport`), `pico/uthernet2.h`; §1cx, §1cy, §1cz, §1az.

---

## 1db-2. USB serial console is a required feature, not just a debug sink (2026-09-01)

**What.** §1da removed `stdio_usb_init()` from `MF_BUS_DIAG` builds. That was wrong: **USB serial is the offline storage upload/download console**, used to check config and move images before the card is installed in the IIc. Removing it broke a real workflow.

**Why the original fix over-reached.** The problem §1da actually needed to solve was that connecting USB *halts the bus loop* (`ReleaseUpdateBusUsbGate()` → `g_release_bus_emulation_enabled=false` → `GetAppleBusBlocking()` spins without draining the FIFO). Killing USB stdio was one way to stop the gate tripping, but it treated a console the user depends on as if it were only a log sink.

**What we did.** `stdio_usb_init()` is restored unconditionally. Only the *gate input* stays neutered in diag builds (`release_usb_console_active()` returns false under `MF_BUS_DIAG`), so:

| Situation | `IsAppleConnected()` | Bus emulation | USB console |
|---|---|---|---|
| On the bench, USB powered | false | off | **`UserTerminal()` runs** — offline storage works |
| Installed in the IIc, USB plugged | true | **on** — storage + U2 work | not entered |

This works because `main()` already guards the terminal with `stdio_usb_connected() && !IsAppleConnected()`, so the Apple check — not the gate — is what keeps the terminal out of bus mode. Diagnostics go to UART, so nothing needs USB to be readable. Verified in the `pico2_diag` disassembly: two `stdio_usb_init` call sites and one `stdio_uart_init`, i.e. both consoles live.

**Residual risk, accepted:** with the gate keyed on `IsAppleConnected()` alone, an installed card with USB attached now runs TinyUSB (USB IRQs + 1 ms `tud_task`) concurrently with the bus loop, which §1da avoided. `behind=0` in the same-generation capture suggests headroom, and `behind` will detect it if that changes.

**References:** `pico/main.c` (both `stdio_usb_init` sites, `UserTerminal()` guards), `pico/misc.c` (`release_usb_console_active`); §1da.

---

## 1dc. Rolled back to the §1cx baseline; §1cx was never committed (2026-09-01)

**What.** The card stopped being detected by the IIc entirely ("megaflash now not found"). At the user's direction we returned the tree to the last state with known-good behaviour — §1cx, where the RECV stalls were cured — to confirm that result reproduces before re-adding any instrumentation.

**The important discovery: §1cx exists only in the working tree.** `HEAD` (`04f798d cleanup`) still contains `u2_poll_counter` and `U2_Poll()`. Every improvement from §1cx onward was uncommitted and interleaved with the §1cy–§1db diagnostics in the same files. A careless `git checkout` would have destroyed the one fix we know works. **Any future rollback request must check this first.**

**Preservation.** `git diff` of all nine touched sources saved to `u2-diag-instrumentation.patch` at the repo root; `git apply u2-diag-instrumentation.patch` restores the full instrumented state. No commits were made. `pico/pico2_diag/` was deleted so the instrumented UF2 cannot be flashed by mistake.

**How the split was done.** §1cx touched only `busloop.c`, `uthernet2.c`, `uthernet2.h`, `uthernet2_net.cpp`. Everything in `CMakeLists.txt`, `busloop.h`, `main.c`, `misc.c`, `misc.h` was introduced by §1cy–§1db, so those five were reverted wholesale with `git checkout` — which removes `MF_BUS_DIAG`, the Release-UART change, and the bus-gate change in one step. The eight `#if MF_BUS_DIAG` blocks in the remaining four files were then removed by hand. Verified two ways: `rg MF_BUS_DIAG` returns nothing, and the residual diff against HEAD is exactly the three §1cx changes (poll-counter deletion, `U2_Poll` → `u2_core0_net_wake_pending`, `U2_BUS_RAM` on the hot path). `nm` on `pico2_release` confirms no diag symbols and `U2_RequestCore0NetPoll` at `0x20001dc8`, i.e. SRAM-resident as §1cx intends.

**Suspects for the detection failure, to be re-added one at a time rather than as a block.** We do not know which change caused it, and the rollback deliberately removed all of them at once:

1. **Per-cycle `MF_BUS_DIAG` work in `BusLoop`** — an APB `flevel` read plus an `fdebug` read/write on U2 cycles. Placed after the chunk-1 publish (§1cz) so it should not affect the prefetch deadline, but it is still work added to the hot loop.
2. **UART stdio enabled in a Release build** (§1da) — this makes every previously-invisible `printf` in Release actually emit and *block* on a 115200 UART. Release normally has no UART sink at all, so any unconditional `printf` on a startup or hot path that was silently discarded before now costs real time. This is the least-examined of the three.
3. **The §1db-2 gate change** — keying `g_release_bus_emulation_enabled` on `IsAppleConnected()` alone. This is the only change that alters behaviour *when USB is attached*, and it is the first time TinyUSB has ever run concurrently with the bus loop in Release. If the failure was observed with a USB cable connected, start here.

**Takeaway.** Three separate mechanisms were bundled into one test build, so a single regression report cannot attribute the fault. Re-introduce them individually, confirming detection after each.

**References:** `u2-diag-instrumentation.patch`; `pico/busloop.c`, `pico/uthernet2.c`, `pico/uthernet2.h`, `pico/uthernet2_net.cpp` (§1cx residue); §1cx, §1cz, §1da, §1db-2.

---

## 1df. An nRESET edge closes every U2 socket mid-transfer; the §1aj guard is not in the code (2026-09-03)

**Capture:** `Serial Saved Output.txt` (2026-09-03), option-B throttled Debug image, 774 lines covering uptime **112.99 s → 202.39 s**.

### The session was healthy, then was killed from outside

```
[u2m] 136563011 sock0 OPEN mr=0x44 port=0 ok       <- MACRAW + MAC filter
   ... 255x "net sock0 MACRAW rx", 249x "sock0 RECV", 81x SEND / 81x MACRAW tx ...
[u2m] 202391572 sock0 RECV                          <- last activity
NETPUMP: RequestAbortAll()
NETPUMP: RequestAbortAll()
[u2macraw] ... x12                                  <- ~120 s, zero U2 activity
```

**~66 seconds of genuinely healthy bidirectional MACRAW traffic** after OPEN: 255 frames in, 81 out, RX dominated by **92 × 784-byte** payloads (bulk HTTP) alongside 63/60-byte control frames. TX ring pointers advance normally (`rd=0x10A7 wr=0x10DD rdm=0xA7 wrm=0xDD`).

Two findings that stand on their own:

- **Zero `RECV STALL` and zero `RECV RESYNC` across 249 RECVs.** §1cx continues to hold; the wedge is gone.
- **Zero "dropped monitor events" warnings**, so the §1de throttled monitor (`U2_MON_FLUSH_MAX=8` @ 460800) keeps up and no longer starves core 0. The Debug build is usable again.

Everything stops dead at the two `RequestAbortAll()` lines. Core 0 stays alive — the 10 s `[u2macraw]` block keeps printing for another ~120 s — but not one further MACRAW frame moves in either direction.

### Root cause: the abort is unconditional, and it closes all four sockets

`gpio_intr_callback` fires on the **falling edge of `nRESET_PIN`** and calls `NetworkPump_RequestAbortAll()` with no qualification. That reaches:

```c
void NetworkPump::RequestAbortAll() {
  INFO_PRINTF("NETPUMP: RequestAbortAll()\n");
  if (activeLegacyOperation != LEGACY_OPERATION_NONE) {
    INFO_PRINTF("NETPUMP: aborting active legacy op=%d ...");   /* log only! */
  }
  session_timers_.clear();
  for (INetworkSession *s : sessions_) { if (s) s->Abort(); }
  UDPTask_RequestAbortIfRunning();
}
```

**The `activeLegacyOperation` test guards only an `INFO_PRINTF`.** It does not gate the abort. So every session is torn down regardless, and `Uthernet2Session::Abort()` is:

```c
void Uthernet2Session::Abort() {
  for (int i = 0; i < U2_NET_MAX_SOCKETS; i++)
    U2_Net_Close(i);
}
```

— it closes **all four sockets**. The 6502 is never told, so ip65 goes on polling `Sn_RX_RD` for data that can no longer arrive.

**This retro-explains the §1db capture.** There, `ptr` sat frozen at `0x0428` (socket 0's `Sn_RX_RD`) while the host issued ~600 `$C0C7` reads per second and nothing arrived. That is exactly the post-`Abort()` state. §1db concluded "the defect is upstream of the bus transport" — correct, and this is the upstream defect.

**The docs asserted a guard that does not exist.** §1aj records "nRESET abort now requires active legacy operation; ignore abort during Uthernet-only sessions so ADTPro MACRAW transfer is not globally torn down". The summary table also lists §10n as "**Reverted:** no `IsLegacyOperationActive`". `IsLegacyOperationActive()` does exist and *is* used in `uthernet2_net.cpp:386` for the ingress decision — but it was never wired into `RequestAbortAll()`. §1aj should be read as intent, not as landed behaviour.

### RESOLVED — not a defect. Operator confirmed a deliberate Ctrl-Reset

The operator confirms the `nRESET` edge at t≈202.4 s was a **deliberate Ctrl-Reset** to quit wget: *"apple reset is end of story. clear and go home."* So the teardown is intended product behaviour, the two `RequestAbortAll()` lines are the correct response to it, and **no fix is wanted**. The `AbortOnAppleReset()` opt-out sketched here was **not implemented** — do not re-propose it.

Two things survive from this section:

1. **The §1db "frozen pointer" reading was a red herring.** That capture almost certainly caught the same post-reset state, not an emulation defect. §1db's conclusion "the defect is upstream of the bus transport" should not be read as evidence for anything.
2. **This capture contains no instance of the actual bug.** The monitor records frame arrival and RECV, never the bytes the 6502 read, so a frame delivered with a corrupt body still logs as a clean `rx` + `RECV` pair. The operator re-confirms the real symptom is unchanged and is the original one: **wget reports a bad checksum on roughly every 3rd or 6th packet.** A clean `[u2m]` log is therefore fully consistent with the bug being present throughout — see §1dg.

**References:** `Serial Saved Output.txt`; `pico/main.c` (`gpio_intr_callback`), `pico/network_pump.cpp` (`RequestAbortAll`), `pico/uthernet2_net.cpp` (`Uthernet2Session::Abort`); §1cx, §1db, §1de, §1dg.

---

## 1dg. H3 confirmed by arithmetic: socket 0's RX ring wraps at 4 KiB but the host's address auto-increment wraps at 8 KiB (2026-09-03)

The frame-size mix in the §1df capture turns the operator's "every 3rd, or 6th packet" into a **measurement**, and it lands exactly on a geometry disagreement that has been sitting in the code the whole time.

### The two wrap boundaries do not coincide

ip65 writes **`RMSR = 0x0A`** (noted at `uthernet2.c:259`; the reset default `0x06` gives socket 0 the same 4 KiB). RMSR is 2 bits per socket, socket 0 in bits [1:0]:

| socket | field | requested | assigned by `u2_apply_socket_sizes` |
|---|---|---|---|
| 0 | `0b10` | 4 KiB | **4 KiB @ `0x6000`–`0x6FFF`** |
| 1 | `0b10` | 4 KiB | 4 KiB @ `0x7000`–`0x7FFF` |
| 2 | `0b00` | 1 KiB | clamped to 0 (8 KiB exhausted) |
| 3 | `0b00` | 1 KiB | clamped to 0 |

**Producer** (`u2_push_rx_macraw`) wraps at the *socket* size: `u2_memory[base + (wr & mask)]`, `mask = receive_size - 1 = 0x0FFF`, so it wraps `0x7000` → `0x6000`.

**Consumer** (`auto_increment`, reached from `read_value` on every `$C0C7` DATA read) wraps only at the *8 KiB memory-block* boundaries:

```c
u2_data_address++;
if (u2_data_address == W5100_RX_BASE || u2_data_address == W5100_MEM_SIZE)  /* 0x6000, 0x8000 */
  u2_data_address -= 0x2000;
```

There is **no wrap at `0x7000`**. So when the host's read pointer walks off the end of socket 0's ring it does not come back to `0x6000` — it marches on into socket 1's region (`0x7000+`), which the producer never writes. The frame's tail was written at `0x6000`; the host reads unrelated bytes instead. **Bad checksum, once per 4 KiB of received data.**

### The frequency is the fingerprint

Corruption should hit one frame per 4096 bytes of RX:

| frame size | 4096 / size | predicted | operator reports |
|---|---|---|---|
| 1514 (full MTU) | 2.7 | every ~3rd packet | **"every 3rd"** |
| 784 (dominant in this capture, 92×) | 5.2 | every ~6th packet | **"or 6th"** |

Both numbers fall out of one constant. This is the strongest correlation obtained so far, and it explains why the corruption is *periodic* yet indifferent to every timing fix attempted — it is a **geometry** bug, not a race, which is why §1cx (real, cured the stalls) and the H2 prefetch work (`behind = 0` over 38,807 reads) left it untouched. It also retro-explains the non-zero **`data_oow = 4`** in §1db, dismissed there as minor: those were reads landing outside socket 0's assigned window, i.e. this exact effect.

### Which side is wrong is still open, and it matters

`auto_increment` is **faithful to AppleWin**, which wraps at `0x6000`/`0x8000` and nothing else. On real W5100 hardware the chip's RX write pointer certainly wraps inside the socket buffer, and the address auto-increment certainly does not — so a correct W5100 driver must **split its read at the socket boundary** and re-point the address register to the buffer base. That leaves two possibilities:

- **(A) ip65 splits correctly.** Then the producer is right, out-of-window reads are impossible, and the frequency match above is a coincidence.
- **(B) ip65 relies on the chip's `0x8000` auto-wrap** and treats socket 0's ring as the full 8 KiB from `0x6000`. Then our 4 KiB producer wrap is the divergence.

The `data_oow = 4` observation is weak evidence for (B), since under (A) it must be exactly zero. Under (B) there is a further consequence worth noting: once the host's `Sn_RX_RD` exceeds `0x6FFF`, our `rd & 0x0FFF` aliases it back to the *start* of the ring, which would wreck the RSR/occupancy math and manufacture exactly the "impossible header" conditions that §1cj/§1cq were built to paper over. That would make this one root cause behind a whole family of symptoms.

### RESULT: REFUTED. The 4 KiB geometry was already correct

The `-DU2_RX_SOCK0_8K=1` image was **dramatically worse**, not better: *"connections fail to setup or get reset by peer. web page download might start, but does not complete. request sends, but does not hear response. many attempts, but only once got a partial contiki page load. did not even get to wget."*

That is the **(A)** outcome, and it is decisive in both directions:

- Under **(B)** — ip65 relying on the chip's `0x8000` auto-wrap and treating socket 0 as an 8 KiB ring — forcing 8 KiB would have made both sides *agree* and the corruption would have disappeared. It did the opposite. **(B) is dead.**
- Under **(A)** — ip65 splitting its reads at the socket boundary it derived from the `RMSR` it wrote — forcing the producer to 8 KiB introduces a brand-new disagreement: our producer walks `0x6000`–`0x7FFF` while ip65 wraps every 4 KiB, and `rd & 0x1FFF` vs the host's 4 KiB pointers wrecks the RSR math. Total breakage, exactly as observed. **(A) confirmed: ip65 manages the socket-boundary wrap itself, and our 4 KiB producer geometry agrees with it.**

**H3 is dead. Do not re-enable this; the code has been removed** (the option was a footgun that produces a badly broken card). The baseline `optionB/megaflash-optionB-pico2w.uf2` is unaffected — the knob defaulted to 0.

### What the failed experiment nevertheless taught us

Two things worth more than the hypothesis that died:

1. **The *character* of the symptom is now diagnostic.** A real, persistent geometry mismatch looks like the 8 KiB build: sessions that will not establish, resets by peer, nothing completing. The actual bug looks nothing like that — the session is healthy, the download progresses, TCP retransmits, and **one frame in every few is rejected for a bad checksum**. That is a **byte-level, one-off defect**, not a structural pointer disagreement. Any future hypothesis must predict *isolated single-frame damage on an otherwise healthy stream*.
2. **The 4 KiB periodicity may still be real, with a different mechanism.** Under (A), the socket-boundary split is the one moment ip65 re-points the address register *mid-frame* (writes to `$C0C5`/`$C0C6` between DATA reads). That happens exactly once per 4 KiB — the same period — so the frequency match may be pointing at the **re-point sequence** rather than at the ring layout. Note `busloop.c` already refreshes the prefetch (`registers.r[7] = U2_PeekDataPort()`) after *every* U2 access including address writes, so simple prefetch staleness is already handled; this is a lead, not a conclusion.

### Superseded: the test that was built

Unify the two boundaries by giving socket 0 the entire 8 KiB RX region (`-DU2_RX_SOCK0_8K=1`) so the producer wrap and the auto-increment wrap are the same boundary. **Built, tested, refuted, removed** — see the RESULT above. Retained here only as the record of the dead end.

<details>
<summary>Original rationale (kept for the record)</summary>

### Decisive test: unify the two boundaries

Rather than add more logging, make the disagreement impossible by construction — give socket 0 the entire 8 KiB RX region so the producer wrap (`0x8000`→`0x6000`) and the auto-increment wrap (`0x8000`→`0x6000`) are the *same boundary*. Gated by `-DU2_RX_SOCK0_8K=1`; RMSR readback stays honest, only internal geometry changes. MACRAW uses socket 0 only, so nothing else is affected.

- **Checksum errors vanish** ⇒ (B) confirmed; then decide the fidelity-correct fix.
- **Checksum errors persist** ⇒ (A); H3 is dead and geometry is exonerated.

</details>

**References:** `pico/uthernet2.c` (`u2_apply_socket_sizes`, `u2_push_rx_macraw`, `auto_increment`, `read_value`, `u2_rx_used_bytes`), `pico/w5100_regs.h` (`W5100_RX_BASE`, `W5100_MEM_SIZE`, `W5100_RMSR`); §1cf, §1cj, §1cq, §1cx, §1db, §1df, §1dh.

---

## 1dh. Stop guessing: audit what we served against what the host says it consumed (2026-09-03)

§1dg burned a flash cycle on a hypothesis about ip65's behaviour that could have been *measured* instead. The remaining live hypothesis, **H1 (dropped bus cycle)**, is stated in the code itself:

```142:148:pico/busloop.c
      /* Nothing else may run here. The a2bus SM serves the next $C0C7 read straight from
       * rxf_putget ~90 ns after nDEVSEL falls, so any work on this path can make the 6502 latch
       * the previous byte (or overflow the 4-deep listener FIFO, which drops the cycle so
       * u2_data_address never advances). Either duplicates a byte and shifts the rest of the
       * frame — the every-3rd/6th-packet checksum failures of §1cx. */
```

A dropped cycle duplicates one byte and shifts the remainder of the frame — **isolated single-frame damage on an otherwise healthy stream**, which is exactly the symptom character §1dg established. So H1 is the hypothesis that fits.

### The measurement is free, because the host already tells us the answer

The host declares how many bytes it believes it consumed: it advances `Sn_RX_RD` by exactly that count before issuing `RECV`. And `read_value()` observes every genuine `$C0C7` DATA read. If the 4-deep `a2buslistener` FIFO discards a cycle, we never see that read and `u2_data_address` never advances — so:

> **observed in-window DATA reads < host's `Sn_RX_RD` advance, and the deficit is precisely the number of lost bus cycles.**

No inference, no correlation, no log archaeology. `-DU2_RX_AUDIT=1` prints every 10 s:

```
[u2audit] frames=N ok=N SHORT=N over=N skip=N lost=N last=D def1=N def2=N def3=N def4=N def5+=N
```

| field | meaning |
|---|---|
| `ok` | reads observed == `Sn_RX_RD` advance — clean delivery |
| **`SHORT`** | `0 < observed < advance` ⇒ **dropped cycles; H1 proven** |
| `over` | observed > advance — host re-read bytes / stale address |
| `skip` | `observed == 0 < advance` — driver discarded a frame unread (**legitimate**, and separated out precisely so it can't be miscounted as a drop) |
| `lost` | total byte deficit across all `SHORT` frames |
| `def1..def5+` | histogram of per-frame deficit; a FIFO drop should sit at **`def1`** |

**Predicted if H1 is true:** `SHORT` climbs at roughly the rate of the corrupt packets — one per 3–6 frames — with the deficit concentrated in `def1`, and `lost ≈ SHORT`.
**If `SHORT` stays 0** while wget still reports bad checksums, then every byte was *counted* correctly and the corruption is in the byte *values* — the fault moves to the PIO/prefetch delivery path (the 6502 latching a stale `rxf_putget` byte), and H1 dies too.

### Implementation notes

- The tally is one range check plus one increment, inlined into `read_value()` (already `__time_critical_func`, so RAM-resident). Deliberately **plain `static inline`, not `U2_BUS_RAM`** — pinning it separately would force a `noinline` call onto the ~90 ns hot path, which is the very thing under investigation.
- The comparison runs in the `RECV` handler, *before* the `sn_rx_rd` shadow is republished, because at that instant `sn_rx_rd` still holds the previous committed RD.
- The prefetch path (`U2_PeekDataPort` → `read_value_at`) bypasses the counter, so speculative peeks are never counted as host reads.
- **Release build on purpose.** The Debug monitor's blocking UART perturbs the exact bus timing being measured (§1cz/§1de), so the audit is gated on `U2_RX_AUDIT` rather than `NDEBUG` and its 10 s print sits outside the `#ifndef NDEBUG` block in `U2_Net_ServicePoll`.
- Release disables UART stdio and a connected USB console gates the bus loop off entirely (§1da), so USB cannot be the sink. `U2_Init` re-runs `stdio_uart_init()` when `U2_RX_AUDIT` is set — kept in `uthernet2.c` so `main.c` needs no change. **Capture on UART at 115200; leave USB unplugged.**

**Image:** `optionB/megaflash-optionB-RXAUDIT-pico2w.uf2` (Release, pico2_w).

**References:** `pico/uthernet2.c` (`u2_audit_note_read`, `read_value`, `write_socket_register` RECV case, `U2_RxAuditReport`, `U2_Init`), `pico/uthernet2.h`, `pico/uthernet2_net.cpp` (`U2_Net_ServicePoll`), `pico/CMakeLists.txt` (`U2_RX_AUDIT`), `pico/busloop.c`; §1cx, §1da, §1db, §1de, §1dg.

---

## 1di. Runtime evidence: the loss is exactly 2 bytes, always at the ring wrap (2026-09-03)

First capture from the `U2_RX_AUDIT` build (`.cursor/debug-a36369.log`, 64 NDJSON records over
~148 s, 201 socket-0 RECVs). Three hypotheses died and one very sharp signature emerged.

### H5 (core 1 falling behind the listener FIFO) — REJECTED

`{"cycles":155358,"backlog":2,"backlog_max":1}`

Backlog on **2 of 155,358** bus cycles, maximum depth **1** of 4. Core 1 keeps up essentially
perfectly, so the 4-deep `a2buslistener` FIFO is **not** discarding cycles. Probe removed from
`busloop.c` rather than left on the timing-critical path it was measuring.

### H4 (core 0 starved by blocking UART writes) — REJECTED

`{"polls":15625065,"gap_max_us":50867,"gap_gt5ms":3,"gap_gt20ms":3,"gap_gt100ms":0}`

15.6 M services in 148 s (~105 k/s), and `gap_max_us` was frozen at 50 ms from the 18 s mark
onward — the three long gaps all happened during Wi-Fi bring-up, before traffic. Never a gap
over 100 ms. **Enabling UART stdio in Release did not cause the browsing stall**, so the audit
build is a valid measurement vehicle.

### H1 (dropped bus cycle) — REJECTED as a mechanism

`over=0`, `skip=0`, and the deficit histogram is **`def1=0, def2=17, def3=0, def4=0, def5plus=0`**.
A discarded cycle loses **one** byte at a time and would land in `def1`. Every single loss here is
**exactly 2 bytes**. Combined with the near-zero FIFO backlog, cycle loss is not what is happening.

### The signature: 2 bytes, 100 % of the time, only at the wrap

`frames=201 ok=184 short=17 lost=34 wrapped=17`

**Every** mismatch (17/17) is a frame whose consumed range crossed the ring end, and **every** one
is short by exactly 2. From the detail records:

| prev_rd | off | advance | seen | deficit | hdr |
|---|---|---|---|---|---|
| `0x0F5D` | `0xF5D` | 426 | 424 | −2 | 426 |
| `0x0D1D` | `0xD1D` | 786 | 784 | −2 | 786 |
| `0x5FE5` | `0xFE5` | 65 | 63 | −2 | 65 |
| `0xFE13` | `0xE13` | 786 | 784 | −2 | 786 |

Two facts fall out:

1. **`advance` == `hdr` == frame_len + 2 in every case.** The host consumes the whole MACRAW
   record (2-byte length + payload) and advances `Sn_RX_RD` by exactly that, so the host's
   accounting is correct and the header we wrote is intact at the wrapped location.
2. **`seen` is always the payload length**, i.e. precisely **2 reads went uncounted**. The audit
   only counts reads inside `[receive_base, receive_base+receive_size)`, so 2 of the host's DATA
   reads landed **outside socket 0's ring**.

### Correction: `Sn_RX_RD` is a free-running counter, not a physical address

`prev_rd` climbs monotonically across the whole 16-bit range — `0x0F5D → 0x1CF9 → 0x5FE5 →
0x9DBF → 0xFE13 → 0x0125` — rather than staying inside `0x6000`–`0x6FFF`. So the comment on
`u2_rx_used_bytes` ("ip65 keeps Sn_RX_RD as a physical W5100 address") is **wrong**: ip65 keeps it
as a virtual byte counter that wraps at 64 K, exactly as the W5100 datasheet specifies, and
derives the physical address as `base + (RD & mask)`. Masking with `0x0FFF` happens to work either
way because `0x6000` is 4 KiB-aligned, so nothing downstream is affected — but the comment should
not mislead the next reader.

### Live hypotheses

- **H7 — reads run past the ring end.** `auto_increment()` wraps only at `0x6000`/`0x8000`, so
  after `0x6FFF` it continues to `0x7000`, `0x7001` — socket 1's region, which the producer never
  writes — before ip65 re-points to the base. Those 2 foreign bytes land mid-frame and break the
  checksum, once per 4 KiB, i.e. every 3rd–6th packet depending on frame size. **This fits every
  number in the capture.**
- **H8 — accounting artifact.** The 2 reads could be in-window but attributed to the adjacent
  RECV interval, in which case there is no real corruption at the wrap and the audit framing is
  what is off. Weak (non-wrapping frames match exactly, 184/184) but it must be excluded, because
  "fixing" a measurement artifact would be worse than the bug.

Next capture records the **actual addresses** of reads outside the ring (`oow_reads` plus the raw
addresses) and the **total** number of wrapping frames. `0x7000`/`0x7001` confirms H7 and makes the
fix a one-line wrap at the socket boundary; zero out-of-window reads kills H7 and leaves H8.

Note the browsing stall is probably the *same* defect rather than a second one: one corrupt frame
per ~12 forces a TCP retransmit, which is a stall-then-burst for an interactive fetch, while
wget's bulk stream absorbs it — matching the operator's report that wget is fine.

**References:** `.cursor/debug-a36369.log`; `pico/uthernet2.c` (`u2_audit_note_read`, `read_value`,
`auto_increment`, `write_socket_register` RECV case, `U2_RxAuditReport`), `pico/uthernet2_net.cpp`,
`pico/busloop.c`; §1cx, §1dg, §1dh.

---

## 1dj. The instrumentation itself broke detection; hot-path budget is ~3 instructions (2026-09-03)

**Symptom:** after the §1di follow-up image, *"uthernet ii network device is not found any more."*

**Cause:** self-inflicted, and exactly the failure mode `busloop.c` already warns about — *"can
corrupt $C0C4–$C0C7 read data and break ip65 W5100 RTR probe → 'Device not found'"*. The §1di
capture added an out-of-window branch plus a ring write with an atomic store to
`u2_audit_note_read`, which is inlined into `read_value` on **every** `$C0C7` DATA read. That read
must complete inside the ~90 ns before the a2bus SM latches the next byte — roughly a dozen cycles
at 150 MHz.

Measured on the ELF, `read_value` instruction counts:

| build | insns | delta vs baseline |
|---|---|---|
| `U2_RX_AUDIT=0` (shipping baseline) | 9 | — |
| §1di capture build (**broke detection**) | 28 | **+19** |
| §1dj rework (this one) | 12 | **+3** |

The audit that produced the good §1di data was the `sz && (addr - base) < sz` one-liner; the extra
branch roughly tripled the function. **Working rule for this path: instrumentation must stay within
a few instructions of baseline, and anything with a store, an atomic, or a second branch belongs
somewhere else.**

### H8 (accounting artifact) — REJECTED without new instrumentation

Re-reading the §1di data settles it: `over = 0` across 201 frames. If the 2 reads had merely been
attributed to a neighbouring RECV interval, that neighbour would have shown `seen == advance + 2`,
i.e. an `over` of exactly 2. Not one frame did. So the 2 reads are **not counted anywhere**, which
means they genuinely landed outside socket 0's ring window. H8 is dead; the loss is real.

### The remaining fork, and why it decides the fix

- **H7** — the host reads 2 bytes past the ring end and *consumes* them. Those come from `0x7000+`,
  which the producer never writes, so 2 foreign bytes land mid-frame → bad checksum. Fix: wrap the
  address at the socket ring boundary.
- **H9** — the host reads 2 bytes past the end, *notices, re-points, and discards them*. Then the
  2-byte deficit is **benign bookkeeping** and the checksum fault is something else entirely. In
  this case wrapping would make the host read the same 2 bytes twice and **duplicate** them —
  strictly worse.

These are indistinguishable from the §1di data, which is why no fix has landed. Two counters, each
one comparison, on paths that already compare the address:

- `hit_end` in `auto_increment` (which already compares against `0x6000`/`0x8000`): the address
  carried off the ring end. Compared against a cached `u2_aud_ring_end` global to avoid a struct
  walk on the hot path.
- `repoint` in the `ADDRESS_HIGH` **write** handler — off the read-critical path entirely: the host
  wrote an address register while already past the ring end.

`hit_end ≈ wrap_total` with `repoint == 0` ⇒ **H7**, and the one-line boundary wrap is correct.
`hit_end > 0` with `repoint ≈ hit_end` ⇒ **H9**, the deficit is a red herring, and the checksum hunt
moves to the byte-value/delivery path with the wrap left alone.

**References:** `pico/uthernet2.c` (`u2_audit_note_read`, `auto_increment`, `u2_apply_socket_sizes`,
`U2_HandleBusAccess` address-write path), `pico/busloop.c` (the ~90 ns warning); §1cx, §1di.

---

## 1dk. Root cause found: the host reads 2 bytes past the ring end and consumes them (2026-09-03)

Second `U2_RX_AUDIT` capture (123 records, 424 socket-0 RECVs, ~380 s). The two probes came back
with a perfect correlation — at **every** sample, across the whole run:

```
hit_end == repoint == wrap_total == short
```

ending at `8 == 8 == 8 == 8`, with `def2=8` and `over=0`. Not one wrapping frame escaped, and
nothing else ever failed.

### What that proves

1. **`wrap_total == hit_end`** — every frame whose data crosses the ring end causes
   `auto_increment` to carry the address off the end. The host does **not** pre-split its read at
   the boundary; it relies on the pointer and only reacts afterwards.
2. **`hit_end == repoint`** — every crossing is followed by the host writing the address registers.
   It notices and re-points.
3. **`wrap_total == short`, deficit always exactly 2** — the arithmetic closes:

   > in-window reads (`seen`) + 2 out-of-window reads == `advance` == `frame_len + 2`

   The host issued exactly the right *number* of reads for the record, but **2 of them were served
   from `0x7000`–`0x7001`** — socket 1's region, which the producer never writes — and it never
   read the 2 real bytes sitting at the wrapped ring base. **Two bytes of every wrapping frame are
   foreign data. That is the bad checksum.**

`over = 0` across 424 frames also re-confirms this is not a bookkeeping artifact (§1dj): had those
2 reads simply been credited to an adjacent RECV, that neighbour would have shown `advance + 2`.

### Rate check against the reported symptom

Every wrapping frame is corrupt, so the corruption rate is just the wrap rate: one per 4 KiB of RX.
With 784-byte frames that is `4096/786 ≈ 5.2` → **one bad frame in ~5**, and with full-MTU frames
`4096/1516 ≈ 2.7` → **one in ~3**. That is the operator's "every 3rd, or 6th packet" exactly. The
observed 8/424 (1.9 %) in this run is lower only because wget never started, so there was little
bulk traffic and few large frames — consistent, not contradictory.

### Reconciling with §1dg

§1dg forced `receive_size` to 8192, which moved the **producer's** wrap, the RSR arithmetic, and the
host's `rd & mask` relationship all at once — a far larger change than the actual defect, and it
broke everything. The real defect is narrow: `auto_increment()` wraps only at `0x6000`/`0x8000`, so
it does not wrap at the **active socket's** ring end. Nothing about the producer or the buffer sizes
is wrong.

### One detail still gates the fix

The obvious fix is to wrap the address at the socket ring boundary. Whether that is a fix or a new
bug depends on the host's **re-point target**:

- target `ring_base + 2` — the host has already accounted for the 2 bytes it consumed, so wrapping
  hands it the correct wrapped bytes and the record is assembled whole. **Fix works.**
- target `ring_base + 0` — the host intends to re-read from the start of the wrapped tail, so
  wrapping would make it read those 2 bytes twice, duplicating them and leaving the frame broken.
  **Fix would be wrong**, and the correct change is elsewhere.

Captured on the address-register **write** path only (`u2_aud_tgt`), armed on the `ADDRESS_HIGH`
write and recorded once the low byte lands. `read_value` stays at 12 instructions versus 9 for the
audit-free baseline — unchanged from §1dj, so the detection regression is not re-introduced.

**References:** `pico/uthernet2.c` (`auto_increment`, `u2_audit_note_read`, `U2_HandleBusAccess`
address-write path, `U2_RxAuditReport`); §1cx, §1dg, §1dh, §1di, §1dj.

---

## 1dl. The re-point target kills the one-line fix; the read counter was blind to the reads in question (2026-09-03)

Third capture (167 records, 205 RECVs). The §1dk decider returned the *unfavourable* answer, and
re-reading the code with it invalidated part of §1dk's reasoning.

### Measured

`hit_end == repoint == wrap_total == short` reproduced exactly a third time (3/3/3/3, `def2=3`,
`over=0`), so the structural finding is solid. The new probe:

| re-point | target | ring base | offset |
|---|---|---|---|
| 1 | 24576 | 24576 | **0** |
| 2 | 24576 | 24576 | **0** |
| 3 | 24576 | 24576 | **0** |

**3 of 3 at `ring_base + 0`.** Per §1dk's own criterion this is the branch where wrapping
`auto_increment` at the ring boundary is *not* the fix: the host would take the first 2 wrapped
bytes at the boundary and then re-read those same 2 bytes after re-pointing to base, duplicating 2
bytes instead of losing 2. The frame stays corrupt, just differently. **Not applied.**

### Why §1dk over-claimed, and the instrumentation defect behind it

§1dk asserted the host reads 2 bytes at `0x7000`–`0x7001`. The evidence does not support that.
`hit_end` fires inside `auto_increment`, which runs *after* a data-port access, so the address
arriving at `0x7000` only proves the host read the last in-ring byte at `0x6FFF`. Whether it then
read *at* `0x7000` or parked there and re-pointed is not distinguished.

The reason it could not be distinguished is a flaw in the audit itself: `u2_audit_note_read()`
counted only reads **inside** socket 0's ring. Reads past the ring end were silently uncounted, so
"host read 2 fewer bytes" and "host read 2 bytes from outside the ring" produced *identical*
counters. The audit was blind to precisely the quantity being measured.

That also matters because a real W5100 would return socket 1's buffer contents at `0x7000` just as
we do, so a driver that genuinely read there would be broken on real hardware too. The pressure is
therefore on the "read 2 fewer" branch, and a fix built on §1dk's assumption would have been wrong.

### Producer cleared

Checked before adding instrumentation. `u2_push_rx()` and `u2_push_rx_macraw()` both address the
ring as `u2_memory[base + (wr & mask)]` per byte, so the write side wraps correctly and cannot be
placing the tail 2 bytes off. No change made.

### Changes

1. **`u2_audit_note_read()` now counts every host DATA read**, window test removed. The total is
   directly comparable to the `Sn_RX_RD` advance, so reads past the ring end can no longer hide.
   `u2_audit_reads` also lost `volatile` (core 1 exclusively; core 0 only sees it packed into the
   trace). Net cost is 4 instructions — a literal load, load, add, store — comparable to the build
   that detected correctly, and the windowed form it replaces was no cheaper.
2. **Bounded pointer trace** replacing the answered re-point probe. Each address-register write
   records `(address, cumulative reads)`; the delta in read count between consecutive entries is
   how many bytes the host pulled from that pointer position, which reconstructs the access
   pattern exactly. It overwrites freely until the address reaches the ring end, then takes 10 more
   entries and freezes, so the 32-entry dump straddles one real wrap with its lead-in intact
   instead of showing the first writes after boot. Write path only; core 0 emits it once, one entry
   per line so a long dump cannot stall core 0 inside a single `printf`.

### What the trace decides

- Pointer parks at ring end with **no** intervening reads, then re-points to base — the host reads
  the right bytes and the 2-byte gap is in the `Sn_RX_RD` advance, i.e. our RSR/RD accounting hands
  it a length 2 larger than the record.
- Pointer sits past ring end **with 2 reads charged to it** — the host really does read foreign
  bytes, and the ring must wrap at the socket boundary despite the re-point target.

**References:** `pico/uthernet2.c` (`u2_audit_note_read`, `auto_increment`, `U2_HandleBusAccess`
address-write path, `u2_trc`, `U2_RxAuditReport`); §1dg, §1dh, §1di, §1dj, §1dk.

---

## 1dm. The trace caught a wrap: the host really does read 2 bytes past the ring end (2026-09-03)

The §1dl pointer trace froze around a real wrap and gave the first direct address evidence.

### The captured wrap

Decoding the pointer targets (`0x0426` = Sn_RX_RSR, `0x0428` = Sn_RX_RD, `0x0401` = Sn_CR), the
driver's per-frame loop is: point at Sn_RX_RSR, read 4 bytes (RSR + RX_RD via auto-increment),
point at the record inside the ring, read the whole record in one burst, point at Sn_RX_RD, write
it back, point at Sn_CR, issue RECV. Two non-wrapping frames in the same trace confirm it — offset
2597 and offset 73, each read as a single 786-byte burst, and the records tile exactly
(2597 + 786 = 3383).

The wrapping frame, entries 21–24:

| entry | pointer | ring offset | cumulative reads |
|---|---|---|---|
| 21 | 27959 | 3383 | 10 |
| 22 | 24576 | **0** | 725 |
| 23 | 1064 (Sn_RX_RD) | – | 796 |

```
burst 1 : 725 - 10 = 715 reads from offset 3383  ->  3383 + 715 = 4098 = ring end + 2
burst 2 : 796 - 725 = 71 reads from offset 0
total   : 786 = the Sn_RX_RD advance
```

**Burst 1 runs 2 bytes past the ring end.** The record needs 713 bytes to the end and a 73-byte
tail; the host took 715 and then 71. So H7 is confirmed on addresses and counts, not inference:
the host reads `0x7000`–`0x7001`, which the producer never writes, and the §1dl caution that it
might merely have parked there is settled.

### Why this still is not enough to fix

The two bursts are self-inconsistent under *any* auto-increment behaviour. Having consumed 715
record bytes, the host should resume at ring offset 2, but it re-points to offset 0 (confirmed
again here, and 3/3 in §1dl). Assemble it either way:

- **as built today** — 713 good bytes, 2 foreign bytes, then offsets 0–70: the tail is shifted by
  2 and offsets 71–72 are never read.
- **with a socket-boundary wrap** — 713 good bytes, offsets 0–1 correct, then offsets 0–70 again:
  offsets 0–1 duplicated, 71–72 still never read.

Both are corrupt, so wrapping remains unjustified and is still not applied. A driver that behaves
this way would fail on a real W5100 too, which returns socket 1's buffer at `0x7000` exactly as we
do. Something in the sequence is therefore still unobserved.

### The blind spot

`u2_trc` recorded **only** `ADDRESS_LOW` writes. A HIGH-only re-point is invisible to it — and that
is precisely how a driver would wrap `0x7000` back to `0x6000`, because the low byte is already
`0x00` and only the high byte needs to change. `g_u2_aud_repoint` counts exactly one HIGH write
past the ring end per wrap, so at least one such write exists in the captured burst. Any invisible
re-point inside a run makes contiguous-looking reads non-contiguous and invalidates the arithmetic
above, which is the most likely explanation for the inconsistency.

### Changes

1. **Trace both address registers**, tagged `hi`/`lo` via `U2_TRC_HIGH`, through a shared
   `u2_trc_note()` on the write path. Depth doubled to 64 (tail 20) to keep the same number of
   pointer moves in view now that each move costs two entries.
2. **Read counting narrowed to the RX block** (`addr >= W5100_RX_BASE`). §1dl's unconditional count
   swept in the Sn_RX_RSR / Sn_RX_RD / Sn_SR reads between frames, putting every frame about 5
   over — 122 of 122 frames reported "over" and the seen-vs-advance comparison became useless. One
   compare against a constant excludes the register file while still counting the region past the
   ring end at `0x7000`, and with no struct load it stays cheaper than the original windowed form.

Cost on the RX read path is 6 instructions (compare, untaken branch, then load/add/store),
comparable to the build that detected correctly.

**References:** `pico/uthernet2.c` (`u2_audit_note_read`, `u2_trc_note`, `U2_HandleBusAccess`
address-write path, `U2_RxAuditReport`); §1dh–§1dl.

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
| MACRAW RX full (W5100 parity) | §1au, `uthernet2.c` `u2_push_rx_macraw` | If **used+(2+len) > size**, new frame **not written**; **no** implicit **RX_RD** flush. (Older doc rows §**1ao**/§**1aq** refer to optional staging / **`into_ring`** paths not present in minimal tree.) |
| MACRAW TX `Sn_TX_RD` vs queue / `pbuf` | §**1ax**, `uthernet2_net.cpp`, `uthernet2.c` `send_data` | **`U2_Net_SendMacraw`** **`0`/`-1`**; advance **`TX_RD`** only on accept; drain **re-queue** on **`u2_send_macraw_core0`** fail |
| MACRAW TX FSR / oversize SEND | §**1cg**→**1ci**, `uthernet2.c` `get_tx_fsr_byte`, `send_data` | §1cg FSR cap **reverted** (report true free — W5100-accurate). Oversize/desync (`data_len>1518`) → **drop** frame (`MACRAW tx len=0` + `OVERSIZE` ptrs), never emit stale-ring garbage; still retire host TX window |
| `Sn_RX_RD` shadow publish point | §**1ch**, `uthernet2.c` `write_socket_register` | Publish `sn_rx_rd` **at `Sn_CR=RECV`** (both bytes final, core 1, order-independent) — **not** on low-byte write (§1cf tore lo-then-hi Contiki writes across 256 B → RECV storm) |
| `Sn_RX_RSR` host-facing vs producer | §**1cj**, `uthernet2.c` `u2_rx_used_bytes_live` / `u2_rx_used_bytes` | Host RSR (`get_rx_rsr`, core 1) = **LIVE** `Sn_RX_RD` from `u2_memory` (shrinks as host drains). Core-0 producer free-space = **shadow** (tear-free, ≤ live ⇒ no overwrite). Shadow-at-RECV froze RSR → Contiki read past `wr` → storm |
| RECV wedge self-heal (freeze **or** creep) | §**1ck→1cl**, `uthernet2.c` RECV handler `bad[]` + `U2_MonRecvResync` | Detect wedge by **impossible header** (`framesize<16 || >rsr`), not by frozen `rd`. Under bulk RX `Sn_RX_RD` **creeps** off-boundary so the old `rd==last_rd` guard never fired → storm ran until 6502 crashed. 3 consecutive impossible-header RECVs ⇒ resync `Sn_RX_RD→Sn_RX_WR` |
| `Sn_RX_RSR`/`Sn_TX_FSR` 16-bit latch | §**1cm**→**1cp**, `uthernet2.c` | §1cm high-byte latch **reverted**: low-first FSR1 saw 0 → first connect/wget failed. Live per-byte FSR/RSR restored |
| VM vs Pico U2 (checksums only on hardware) | §**1cn**→**1cp**, megaflash-vm `host_uthernet.c` vs Pico | VM **host-completes** `$C0C4–$C0C7`. **Do not** skip `U2_Poll` on `$C0C7`; **do not** port overlay latch/have. Hardware-only: PIO prefetch + CYW43 no TAP pause |
| `RECV RESYNC` vs wget second OPEN | §**1cq**, `uthernet2.c` | `12:53:57`: wget STALL/RESYNC discarded 2 KiB after second OPEN. **Log RESYNC, do not rewrite `RX_RD`**. First HTTP can succeed after SYN RTO |
| CYW43 ACK-then-drop vs TAP pause | §**1cr**→**1cs** | Skip-poll **reverted** (no connect/wget change). 1027 pcap: **0** frames to port **1026**; wget RST after good 730 B HTTP |
| STA IPv4 masquerade | §**1ct**→**1cv**, `uthernet2_net.cpp` | **Reverted.** Dual-IP does not explain checksums; NAT would break Contiki as a server. Keep SA/SHA/chaddr only |
| UART `16:17:06` browse STALL | §**1cw** | 3×784 + 424 then `hdr=11BC`→`4000`; `RX_RD` += 0x4000. Not wget/wrap-only |
| UART `16:17:06` browse STALL | §**1cw** | 3×784 + 424 then `hdr=11BC`→`4000`; `RX_RD` += 0x4000. Not wget/wrap-only |
| ADTProETH timeout vs captures | §**1aw** | **`rx_noroom`** = §**1au** **drops** (W5100-sized RX full); not fixed by bigger fake buffer; **`tcpdump`** UDP/6502 OK; UART **460800** |
| UART vs “Device not found” | §1c, `debug/*.log` | `w5100.s` `init` only `SEC`s on RTR XOR; correct RTR reads ⇒ that run passed Ethernet init; **`ip65_init` then `clc`s unconditionally** — see §1c if UI still says device not found; **48× `DATA read`** after `mode=0x03` traces RMSR/SHAR/OPEN |
| UART boot identity | `main.c`, `build_id.h.in`, `CMakeLists.txt` | After reboot, scroll to **latest** `Megaflash DEBUG Firmware…` block: includes **`FIRMWAREVERSTR`** and **`Firmware build:`** (UTC + Unix s from CMake **`–DFIRMWARE_BUILD_TIMESTAMP`**). **`./build-debug.sh`** refreshes those vars each configure so the UF2 matches the UART line; **`cmake --build` alone** can leave a stale **`build_id.h`**. This stamp is **configure-time wall clock**, not the git commit author date — correlate binaries with the **build script run**, not only **`git log`**. |
| ip65 init bisect (Pico 2 W Debug) | `CMakeLists.txt` `U2_IP65_CHECKPOINT`, `uthernet2.c`, `u2_monitor.c` | Default **quiet**: one **`[u2] ck=n`** per run when **`U2_IP65_CHECKPOINT=n`** (1=MODE 0x03 … 5=MACRAW OPEN). Optional **`U2_IP65_TRACE_DATA`**, **`U2_MON_LOG_BUS`** (floods UART) |
| ADTPro crash triage (`system-$01`) | §1p, `debug/uart_log.txt` | If no `sock0 OPEN/SEND/RECV` or checkpoint lines appear in failure window, treat as pre-W5100 crash/handoff issue first; use `U2_IP65_CHECKPOINT=1` build to confirm first mode write is reached |
| Apple readback for U2 | `busloop.c` U2 branch, `a2bus.h` | **`registers.r[4..7]`** → **`i32[1]`** chunk **1** → **`UpdateMegaFlashRegisters(1,…)`**; RP2350 waits for IRQ0 before update (§1d) so SM1 presents the merged byte on the next cycle |
| U2 read data path | `busloop.c`, `a2bus.h` | U2 read byte must update **chunk 1** (SM1), not chunk 0 |
| RP2350 U2 vs PIO IRQ 0 | `busloop.c` §1d | U2 branch must wait for IRQ 0 clear before `UpdateMegaFlashRegisters(1,…)` (same as main loop); skipping caused bad C0C4–C0C7 reads |
| U2 `[u2m]` monitor | `u2_monitor.c`, `build-debug.sh` §1e | Debug-only queued UART trace; flush from `U2_Poll`; bus + socket + net hooks. **§1cz: at 115200 the 48-event flush blocks core 0 for ~330 ms/iteration and starves lwIP → TCP connects fail. Debug is not valid for connectivity or bus timing.** |
| Bus-path counters (`behind`/`rxstall`/`data_oow`) | §**1cz**, `CMakeLists.txt` `MF_BUS_DIAG`, `busloop.c`, `uthernet2.c` | Enable in **Release**: `-DCMAKE_BUILD_TYPE=Release -DMF_BUS_DIAG=1` (`pico2_diag`). Sample **after** `UpdateMegaFlashRegisters(1,…)`; RXSTALL harvested on core 0 (sticky) so the bus path stays clean |
| **§1cx is UNCOMMITTED** | §**1dc**, `u2-diag-instrumentation.patch` | `HEAD` `04f798d` still has `u2_poll_counter`/`U2_Poll()`. The only copy of the known-good no-stalls state is the **working tree** — check before any `git checkout`. Instrumented state preserved in the root patch file |
| **H2 prefetch turnaround — CLOSED** | §**1db**, `Serial Saved Output.txt` | `behind=0` / `fifo_hw=0` over **38,807** `$C0C7` reads ⇒ core 1 always meets the ~140 ns chunk-1 deadline. **Do not do the `a2bus_rp2350.pio` TX-FIFO rewrite.** `data_oow=4` (tiny, stopped early) ⇒ H3 not the main event |
| RX ingest visibility | §**1db**, `uthernet2.c` `U2_DiagReport` | `rx_push` / `rx_drop` / `rx_filt` / `recv` / `send` + per-socket `Sn_MR`/`Sn_SR`/`rd`/`wr`/`rsr`. Added because the bus-path counters could not see a host poll loop parked on `Sn_RX_RD` (`ptr=0x0428`) with no data arriving |
| USB serial = storage console | §**1db-2**, `main.c`, `misc.c` | **Not just a log sink** — offline upload/download before install. `stdio_usb_init()` always called; only the *gate input* is neutered under `MF_BUS_DIAG`, so bench USB → `UserTerminal()`, installed + USB → bus stays live |
| Release stdio sink / USB-vs-bus gate | §**1da**, `main.c`, `misc.c`, `a2bus.h` | Release **disables `stdio_uart`** → USB CDC is the only sink; and `GetAppleBusBlocking()` **spins without draining the FIFO** while `!g_release_bus_emulation_enabled`, which `ReleaseUpdateBusUsbGate()` clears whenever USB is connected. So **USB kills storage + U2 by design**. `MF_BUS_DIAG` builds: UART enabled, `stdio_usb_init()` **not called** (linker drops it), gate ignores USB → USB is power-only |
| A0–A3, nDEVSEL pulls | `a2bus_rp2040.pio`, `a2bus_rp2350.pio` | A2=GPIO8, A3=GPIO9, no pulls; nDEVSEL pull-up on; data bus pull-up |
| Build SDK path | `cmakeall.sh`, `CMakeLists.txt` | Script passes `-DPICO_SDK_PATH`; `CMakeLists.txt` uses **`$HOME/pico-sdk`** when env or `-D` unset (§4); SDK is same git repo on all host architectures |
| Host CMake | `build-env.sh`, `cmakeall.sh`, `build-both.sh`, `build-debug.sh` | **`CMAKE_BIN`**: `/opt/homebrew/bin/cmake` → `/usr/local/bin/cmake` → **`PATH`**; **`CMAKE`** env override |
| Debug build → git marker | `build-env.sh` `mf_debug_build_git_commit`, `build-debug.sh`, `build-debug-both.sh` | After a **successful** debug build: **`git commit --allow-empty`** at **MegaFlash** repo root (UF2 trees are **gitignored**). Message includes **HEAD**, **branch**, **clean/dirty**, host uname, and **`build-debug-both.sh`**-specific **`FIRMWARE_BUILD_TIMESTAMP*`** / **`U2_*`** CMake env lines. **Opt out:** **`MF_DEBUG_BUILD_NO_GIT_COMMIT=1`**. Commit failure **does not** fail the build (warns only). |
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
| NOR flash SI / vendor skew | §23, `pico/flash_si_test/*`, `datasheets/` | Winbond vs Alliance AC deltas; **`flash_si_test`**: baud sweep + SR3 A/B + **soft-SPI** (`35h`/`15h` SR2/3 + `0Ch`) + **XMODEM-like** DMA program/verify (`xmodem_like.c`, **BITINVERSION**) |
| Flash validate (Applesoft + C soak) | `tools/flash-validate/` | §17-18 + §19 + §20 + §21 + §22: `FLASHVAL.BAS` baseline + `FLASHSOAK.BAS` overnight CSV/TFTP + `TFTPUTIL.BAS` (80-col startup, auto slot detect preferring 4, host FQDN/IP prompt + `TFTPUTIL.CFG` default persistence, volume list by unit number + name before selection); `TFTPUTIL.TXT` shipped to disk as `TFTPUTIL.DOC`; `FLASHSOAK/flashsoak.c` + `Makefile` (cc65 → `flashsoak.bin`); `build-flashval-disk.sh` → `FLASHVALID.po` |
| WGET65V on-screen ip65 trace | `tools/wget65-verbose/`, §1h | Fork of `wget65` + register dumps; **`eth_init`** fixed **slot 4** (`$C0C4`); optional **`WGET65V`** on **`FLASHVALID.po`** when **`wget65v.bin`** built locally |
| Drives Enable toggles | `cpanel/drivesenable.c` | `gotoxy` Y is WNDTOP-relative; do not add `YPOS` (§10d) |
| Git 1.1.x patches | branch `1.1.x` | `checkout 1.1.x` to patch/build; `checkout main` to resume tip (§10e) |
| NetworkPump entry | `network_pump.cpp`, `network.cpp`, `main.c` | `RunNTP` / `RunTestWifi` / `RunTFTP` register a short-lived `LegacyUdpSessionAdapter` and spin `PollOnce()` until `GetCompleted()`; `CUDPTask::Run()` still wraps `EnterRunSession` + same loop for any direct caller; Core 0 idle `NetworkPump_PollOnce` (§14.8) |
| lwIP DNS/UDP vs `runningObject` | `udptask.cpp`, `network_pump.{h,cpp}` | DNS: `dns_pending_owner_` (`INetworkSession*`) + `OnDnsGetHostByNameResult` (§14.11), with pending-owner armed before `dns_gethostbyname` to avoid fast-callback race/timeouts. UDP: `NetworkPump_LegacyUdpRecv` + pcb→`INetworkSession*` (`udp_pcb_owners_`); `OnUdpRecvPbuf(pcb,p,…)` → `NotifyUdpReceived` or U2 (§14.10, §14.10b) |
| Uthernet II lwIP | `uthernet2_net.{h,cpp}`, `uthernet2.c`, `main.c` | **`u2_netif_input_wrapper`**: §**10w** — MACRAW-only ingress when sock0 MACRAW + not legacy; lwIP-only when **`IsLegacyOperationActive()`**; **`U2_Net_ServicePoll`** from **`PollOnce`**. **`main.c` §10x**: poll-before-FIFO, **`core0Loop`** FIFO **0**, **`U2_RequestCore0NetPoll`** on SEND/RECV. **`U2_Net_Poll`** → **`PollOnce`**. §**1ar** duplicate feed superseded for coexistence. |
| ADTPro socket-pointer tracing | `uthernet2.c`, `u2_monitor.{h,c}` | Added `sock ptrs` monitor snapshots for `send-pre/send-post/recv-pre` with `TX_RD/TX_WR/RX_RD/RX_WR/SR`; later added coherent `RX_RD` reads (high/low/high retry) to avoid torn cross-core pointer values in `RX_RSR` and trace output (§1ai) |
| Telnet65 walkthrough + fixes | §10f, `uthernet2.c`, `uthernet2_net.cpp`, `network_pump.cpp` | Implemented: TX wrap fix (`<0`), UDP/TCP chained pbuf flattening (`pbuf_copy_partial`), removed duplicate lwIP lock in `U2_Net_OpenUdp`; later `wget65` follow-ups changed RECV to preserve unread bytes (§10h) |
| Other ip65 tools walkthrough | §10g, `ip65/apps/*`, `ip65/apps/w5100.c`, `uthernet2*.{c,cpp}` | Risks: RECV discards unread bytes (shared-access mismatch), TCP RX overflow can drop+ACK, SEND chunk currently capped at 2048; `wget65` highest risk |
| `wget65` priority fixes | §10h, `uthernet2.c`, `ip65/apps/w5100.c` | SN_CR=RECV no longer forces `RX_RD->WR`; TCP SEND drains entire queued TX data in chunks (not single 2 KiB cap) |
| TCP RX backpressure | §10i, §**10j**, §**10n**, `uthernet2_net.h`, `uthernet2.c`, `uthernet2_net.cpp` | §10i: `tcp_recved` only for accepted bytes; **§10j re-landed:** TCP ring **all-or-nothing**; **`ERR_MEM`** + **`refused_data`**; **`U2_Net_RecvConfirm`** flag + core-0 retry |
| TCP TX backpressure | §**10j**, §**10n**, `uthernet2.c`, `uthernet2_net.cpp` | **§10j re-landed:** **`U2_Net_SendTcp`** returns accepted bytes; **`Sn_TX_RD`** advances only on success; **`U2_TryCompletePendingSends`** (**TCP ESTABLISHED** only) |
| TCP TX accounting + UDP no-truncation (2026-07) | §**1bb**, `uthernet2.c` `send_data`, `uthernet2_net.{h,cpp}` | Re-land on current API: **`U2_Net_SendTcp`**→`int` clamps to **`tcp_sndbuf`**; **`send_data`** advances **`Sn_TX_RD`** by **accepted bytes** (remainder stays in FIFO ring, flushes in order next SEND). **`U2_Net_SendUdp`** copies from TX ring into pbuf (no 2 KiB truncation, no big buffer). RP2040 **Debug** pre-existing **460 B** RAM overflow (unrelated) |
| U2 core-0 poll cadence | §**10u** (Phase 1), `ipc.h`, `main.c`, `uthernet2.c` | **`IPCCMD_NET_WAKE`** 1/ms from core 1; **`core0Loop`** FIFO **5 ms**; §10l otherwise unchanged |
| Contiki wget / TCP | §**10l** only (`7adb4ee`), `uthernet2.c` | RX ring geometry; §10j–§10p **reverted** — re-land incrementally |
| MegaFlash native NTP/DHCP vs U2 | ~~§**10m**~~ §**10n**, `network_pump.cpp`, `uthernet2_net.cpp` | **Reverted:** no **`IsLegacyOperationActive`** / **`U2_Net_ServicePoll`** |
| Open C0C4 unresolved item | §12 | Slot decode confirmed working; remaining investigation is nDEVSEL signal/timing visibility at Pico/PIO point vs timing/FIFO/CPU-drain behavior |
| Pump TCP + session timers | `network_pump.{h,cpp}` | `CreateTcpPcb`: `tcp_arg(owner)`, `NetworkPump_LegacyTcpRecv` / `NetworkPump_LegacyTcpErr` → `OnTcpRecvPbuf` / `OnTcpErr`; `tcp_pcb_owners_` for unregister. `ScheduleTimer` / `CancelTimer`; `PollOnce` → `DrainSessionTimers` → `OnTimer` (§14.12) |
| Reset-abort scope during U2 transfers | `main.c`, `network.{h,cpp}`, `network_pump.h` | Confirmed `nRESET` abort now requires active legacy operation; ignore abort during Uthernet-only sessions so ADTPro MACRAW transfer is not globally torn down (§1aj) |
| ADTPro mid-transfer stall (May 2026 capture) | §1ak, `debug/tcpdump.txt` | Send path (Mac→IIc) confirmed by operator; pcap src/dst for large UDP must be reconciled with Mac/IIc IPs; ~721 ms gap after shortened large payload (501→333 B); ~25× ~500 B large payloads before anomaly |
| Wire vs MACRAW **length** / **host** correlation | §1ap, `debug/tcpdump.txt`, UART **`MACRAW host`** | Ingress study used **`MACRAW rx len`** (removed). **Debug** UART: **`MACRAW host eth_len`** at **RECV** (+ **fnv**, sip→dip) vs **`tcpdump`** `length N`; **fnv** = FNV‑1a over Ethernet bytes in ring |
| ADTPro Send RC / remediation procedure | §1am, §1aq, `[u2macraw-rx]` / `[u2macraw-stage]` UART | Ring parity in **`into_ring`**; staging non-zero if FIFO used; **`reject_no_room`** only if staging also full |
| P0-3 RX cross-core | §1al, `uthernet2.c`, `Uthernet-II-stack-architecture-and-todos.md` | `_Atomic` `sn_rx_rd`/`sn_rx_wr`; single **release** publish of `sn_rx_wr` after each UDP/TCP/MACRAW enqueue; **acquire** loads in `get_rx_rsr`; **UDP/TCP** one reload if ring appears full / TCP starved |
| Inbound parity implementation (2026-05) | §1az, `uthernet2.c`, `u2_monitor.{h,c}`, `CMakeLists.txt`, `build-debug-both.sh` | W5100 drop-new MACRAW when full; **`sn_rx_wr`** remap on **`RMSR`** (not zero); **`Sn_TX_FSR`** safe when **`transmit_size==0`**; optional **`U2_MACRAW_COMPAT_DROP_OLDEST`**; **`U2_MonNetRxDrop`** rate-limited; atomic **`sn_rx_wr`** + sizing helper + `[u2m]` RX-drop telemetry |
| U2 stack architecture + TODO / validation | `docs/Uthernet-II-stack-architecture-and-todos.md` | Mermaid diagrams, contract table, P0–P2 issues, validation checklists for stabilization work |
| Apple IIc logo renders (HGR/DHGR/DLGR) | §24, `pico/assets/apple2-logo/` | Quantized previews + HGR `$2000` dump; DHGR uses 140×192 color cells → 560×192 |

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

**Note (XMODEM image upload vs format):** USB receive (`filetransfer.c` → `PacketReceived`) ACKs each valid CRC packet **before** `WriteBlockForImageTransfer()` finishes programming (`mediaaccess.c`); flash writes use `tsProgramOnePage` / Fast Read verify (`flash.c`), same SPI stack as format/erase. The Alliance **AS25F3512MQ** datasheet (Ver 1.0 Aug 2024) documents the same class of commands (**50h** volatile SR enable, **11h**/ **15h** SR‑3 write/read, **B7** 4‑byte mode, **12h** page program, **0Ch** fast read with dummy, **DCh** 64 KB erase) and DRV1/DRV0 coding compatible with **75 %** drive (**0,1** = default). If uploads show **`Verification Error:`** nonzero while Winbond passes, first suspects remain **marginal SR‑3 masking** in `SetFlashDriveStrength()` (still unguarded by manufacturer) or **electrical/SI** at final SPI speed—not a separate XMODEM protocol layer. **AC timing comparison vs Winbond and a Pico-side discrimination procedure** are in **§23**.

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

---

## 23. NOR flash timing (Winbond vs Alliance) and SI discrimination tests

**What:** Operators reported **Winbond W25Q512JV** reliable on **flash1** (CS0, longer traces) while **Alliance AS25F3512MQ** misbehaves there; both parts work on **flash2** (CS1, shorter traces). Need datasheet-backed reasoning and a **repeatable way to tell** “marginal MISO/setup” vs “SR3 / software” vs “DMA-only”.

**Why (datasheets, same AC framing):** Both parts specify SPI AC timing with **CL = 30 pF** at the device pins (Winbond §9.5; Alliance “AC Measurement Conditions”). Under that framing, **Alliance allows a slower worst-case `tCLQV` (clock low → output valid)** than Winbond at the same load (**7 ns vs 6.5 ns max @ 30 pF**; Alliance also quotes **6 ns @ 15 pF**). Alliance specifies **shorter minimum output hold `tCLQX` / `tHO` (1 ns vs 1.5 ns)**, which shrinks the stable eye at the receiver when reflections or extra delay are present. Alliance also requires **longer `/CS` active hold and not-active setup vs CLK (`tCHSH`, `tSHCH` = 5 ns vs Winbond 3 ns)**—layout-dependent if CS and SCK skew differently per socket. Headline **fc** for fast read remains **133 MHz** class on both; the practical difference on a long MISO stub is more about **output delay/hold and SI** than MHz alone.

**What we did:** Documented the above and added an **optional CMake target** **`flash_si_test`**: a minimal UF2 that uses the **same pins, SPI mode 3, and `0Ch` + 1-byte dummy fast read** as `flash.c`, sweeps SPI baud, and counts read mismatches vs a **10 MHz reference** buffer. It prints **CSV lines** (`baud_target,baud_actual,failures/N`) for each **CS0 and CS1** that responds to JEDEC, first with **boot SR3**, then after **volatile SR3 write** with **DRV bits cleared** (`SR3 & 0x9F`, Winbond-style “strongest” output table entry—verify on Alliance if anomalies appear), then restores SR3. After the read sweep, the UF2 runs an **XMODEM-image-style destructive sequence** (`flash_si_test/xmodem_like.c`, links **`dmamemops.c`**): **`DCh`** erase of the **last 64 KiB** of a **64 MiB** map (`0x03FF0000`), then **16×** **`tsWriteOneBlockWithoutErase`-equivalent** steps (**`CopyMemoryAlignedBG`** DMA CRC of source, two **`0x12`** page programs, **`ReadFromFlashByDMA`** verify CRC match). Source bytes match **`WriteBlockForImageTransfer`** via **`BITINVERSION=1`** (invert into a wire buffer before program, same as `tsWriteOneBlockAlreadyErased_Public` in `flash.c`). **Warning:** overwrites tail of flash; adjust **`TEST_SECTOR_BASE`** for non‑64 MiB parts.

**Test algorithm (recommended order):**

1. **Host / board sanity:** Confirm **3V3**, CS idling high, no contention on MISO when both flashes are populated. Compare **scope** SCK/MISO at CS0 vs CS1 at production **75 MHz** (see `SPI_SPEED_FINAL` in `flash.c`): edge-to-edge data valid window, overshoot, and time from **SCK falling** to **MISO stable** vs bit period.
2. **JEDEC and 4-byte mode:** `9F` read on each CS; `B7` enter 4-byte address mode (matches `InitFlash()`).
3. **Stable reference at low speed:** At **10 MHz**, read **512 bytes** from **address 0** with **`0Ch` + 32-bit address + 1 dummy**; repeat until **consecutive reads match** (rules out intermittent open or wrong CS).
4. **Baud staircase (blocking SPI):** Increase `spi_set_baudrate` through a ladder (e.g. 10 → … → 75 → … → 100 MHz **requested**, log **actual** baud from `spi_get_baudrate`). At each step, run **many** identical reads and **`memcmp` to reference**. **First baud with any mismatch** estimates the **SI-limited** ceiling for that chip + trace. Run **A/B**: Winbond vs Alliance on **same socket**, and **CS0 vs CS1** with **same part** if possible.
5. **SR3 output-drive experiment:** If failures **only at high SPI** and **clearing DRV** (or setting production **75%** drive in `SetFlashDriveStrength`) **raises** the passing baud on CS0, weight **output strength / line RC**; if **no change**, weight **setup/hold, /CS skew, or dummy/read path** (e.g. try **2 dummy bytes** once—production uses **1**; a mismatch here would point to **command timing**, not raw MISO RC).
6. **DMA path (optional extension):** Production reads use **`ReadFromFlashByDMA()`** with a **timeout**. Re-run the same sweep using the **DMA+TX-stuff** read path (or temporarily lower `SPI_SPEED_FINAL` in firmware) to separate **“MISO wrong”** from **“DMA timeout / FIFO”**.
7. **`flash_si_test` XMODEM-like phase:** If blocking reads pass but **USB XMODEM** fails verification, run the UF2’s second phase (or build/run **`flash_si_test`** alone after commenting the read sweep if UART noise is an issue). Failures isolate **program + DMA verify** vs **blocking read-only** paths.

**Interpretation cheat-sheet:** CS0 fails high SPI but CS1 passes with the **same chip** → **layout / capacitance / reflections / CS–SCK skew**. If **`flash_si_test`** shows **CS0 XMODEM-like failures at ~9 MHz as well as 75 MHz** (same **`probe_nonff`**, bogus **`SR=00`**), weight **CS0 net / MISO / `/CS` / false `wait_busy`** over **SPI clock margin alone**. Alliance fails but Winbond passes on **same layout** at same baud → consistent with **datasheet `tCLQV` / `tHO` deltas** and **process spread**, not a different command set. SR3 experiment moves the ceiling → **drive strength** helps; no movement → **host setup or trace** first.

**Observed lab result (2026-05, `flash_si_test`):** Blocking **read sweep** at address **0** reported **0 failures** on **both** CS0 and CS1 (including **75 MHz actual**). The **XMODEM-like** phase (**`DCh`** erase of **`0x03FF0000`**, **`BITINVERSION`**, DMA CRC + **`0x12`** ×2 + **`ReadFromFlashByDMA`** verify) then reported **16/16 failures on CS0** and **0/16 on CS1** — reproducing **“long-trace socket only”** under **program + DMA verify**, not under **short reads from address 0 alone**. **CS0 SR3 readback stuck at `FF`** while **CS1** returns **`21`** remains a red flag for **short-status / MISO** behaviour on that CS net (not definitive of SR contents). **Next:** re-run UF2 after **`xmodem_like.c`** triage lines (**`post-erase probe64`**, **`triage bn0:`** with `crc2_dma_sniff` vs **`crc2_blocking`**) to see whether failure is **erase**, **program**, or **DMA read path only**.

**Triage follow-up (same run, decoded):** **`post-erase probe64`** on **CS0** reported **`non-FF bytes=64`** — the **64 KiB erase did not leave `FF` in the first 64 bytes** at **`0x03FF0000`** (erase not effective on that CS, wrong address window, or **BUSY polling exited while erase still running / bogus status**). On **CS1**, **`non-FF bytes=0`** — erase **did** blank that window. **`dma_rx_ok=1`** and **`crc2_dma_sniff == crc2_blocking`** — verify is **not** “DMA-only”; blocking read agrees with DMA read. **`crc1_dma_repeat == crc_src_plain`** — source CRC path is self-consistent. So the mismatch is **flash content ≠ programmed pattern**, consistent with **never getting a clean erased + programmed image on CS0** at that address. **Firmware:** after erase the test now prints **SR1/SR2/SR3** and **probe first8** for the next log (BP/CMP/WEL/BUSY context). **SPI sweep:** XMODEM-like **erase + probe + BLOCK0** program/verify repeats at **each** baud in the same ladder as the read sweep (CSV **`xfer,spi_want,spi_got,CS,SR1,SR2,SR3,probe_nonff,block0`**); **BLOCK1..15** + triage run only when **`spi_want=75MHz`** (production). **`read_from_flash_dma`** recomputes RX DMA **timeout** whenever SPI baud changes.

**SPI baud sweep (2026-05, CSV decode):** On **CS0**, **every** ladder step (**`spi_got`** from **~9.37 MHz** through **75 MHz**) shows **`probe_nonff=64`**, **`block0=FAIL`**, and **`SR1=SR2=SR3=00`**. On **CS1**, **every** row shows **`probe_nonff=0`**, **`block0=OK`**, and **`SR1=00 SR2=02 SR3=21`**. **Reproducible on two boards**, with **Winbond W25Q512JV** (not Alliance-only). **Conclusion:** the CS0 tail-sector failure is **not specific to 75 MHz SPI** under this test — it reproduces at **the lowest** bit rates too. The **all-zero status** on CS0 during xfer (vs plausible **`SR2=02` / `SR3=21`** on CS1) strongly suggests **bogus MISO / status reads on the CS0 path** (e.g. line stuck low, wrong device selected, or **`wait_busy`** exiting on a **false “not busy”**), not a marginal **fc** limit at one clock.

**Pico-only follow-up diagnostics (no LA / scope required):** Because we cannot capture a 75 MHz waveform with the available equipment, **`flash_si_test`** was extended to push the CS0 vs CS1 split into ranges where SI is not the variable. **UART sanity:** builds print **`build …`** (CMake timestamp, often **`unknown`** unless **`build-both.sh`** sets the cache vars) plus **`compiled __DATE__ __TIME__`**; expect **`=== Soft SPI GPIO`**, **`xfer` row `0,500000,…`**, and **`postErase … SR2`/`SR3`** lines. Logs that jump from **`read sweep done`** to **`XMODEM-like`** with **`0,10000000`** are **older UF2s**.

1. **Soft-SPI GPIO at ~100 kHz (`flash_si_softspi.c`).** **`spi_deinit(spi0)`**, then bit-bang the **same** CS/SCK/MOSI/MISO pins (CPOL0 CPHA1) at a half-bit delay of **5 µs** (~100 kHz SCK). The test prints **JEDEC**, **`SR1`/`SR2`/`SR3`**, **8 B** of **`0Ch`** at address **0** and at the tail **`0x03FF0000`**, and an **`06h`** **WEN** → **`SR1`** (**WEL** bit check) → **`04h`** **WRDI** sequence per CS. **Why this matters:** If CS0 still returns **all `FF`** or **all `00`** at 100 kHz when CS1 returns plausible bytes, the failure is **not an SI / clock-margin issue at all** — the **CS0 net or the second-flash chip-select signal itself** is wrong. If both CS lines return the **same bytes** at 100 kHz, the **JEDEC / MISO / CS routing is fine** and the production-SPI failure is **path-specific** (RP2350 SPI peripheral, DMA, or trace SI at high `fc`). Reads of **0x9F / 0x05 / 0x0C** are tiny enough that even a poor MISO net should resolve at this clock.

2. **Post-erase SR1 transactional vs held-CS bursts (`xmodem_like.c`).** On the **slowest** sweep row (now also includes **500 kHz** and **1 MHz** rows added to the ladder), after the **`DCh`** erase the test prints:
   - **`postErase SR1 tx16+2ms gaps`** — sixteen **independent** `05h` transactions with **2 ms** spacing (CS toggles between each). For **idle** flash after erase, **`SR1=00`** (BUSY clear, WEL clear) on **both** CS is **expected** — this line does **not** discriminate CS0 vs CS1 by itself.
   - **`heldBurst12`** — one CS-low assert, **`05h`**, then **12** consecutive **`SR1`** byte reads. **All `00`** on **both** CS is again consistent with **idle `SR1`**, not proof of a stuck MISO on CS0 alone.
   - **`postErase transactional SR2`/`SR3`** — printed immediately after the burst; compare to CSV **`SR2`/`SR3`** on the same row. **CS0 vs CS1** split here is the primary **HW-SPI** discriminator after erase.

3. **Slow-baud rows.** **500 kHz** and **1 MHz** are now first in `xfer_spi_bauds`. If CS0 still shows **`probe_nonff=64`** at those, it is essentially impossible for **`tCLQV` / `tHO` / fc margin** to be the cause.

**Soft-SPI lab decode (2026-05, two-chip compare, Winbond `20 40 20` on both CS):** **Soft-SPI** showed **identical JEDEC**, **identical first 8 B** of **`0Ch @0h`**, **both** **`SR1=00`** before **`06h`**, **both** **`SR1=02` after `06h`** (WEL works on each chip), and **different** tail **`0Ch @03FF0000h`**: **CS0** first 8 B all **`FF`** (blank tail window), **CS1** **`FF FF 58 59…`** (leftover programmed bytes — content history only). So **CS0 is not “globally broken”** at ~100 kHz: **MISO, `9Fh`, `0Ch`, `06h` all behave**. The failure is **specific to the HW-SPI + erase/probe/program path** on **CS0**, not “cannot talk to flash2 at all.” **Refine SR1-only forensics:** **`postErase` sixteen transactional `SR1` reads and `heldBurst12` showing all `00` on *both* CS** is consistent with **idle `SR1`** (BUSY=0, WEL=0) after a **completed** erase — it does **not** prove **`wait_busy`** is wrong by itself. The strong **CS0 vs CS1** contrast in the same UART row is **`SR2`/`SR3`**: CSV still has **CS0 `00,00,00`** vs **CS1 `00,02,21`** at **500 kHz** — either **HW SPI returns wrong bytes for `35h`/`15h` on CS0** after that transaction sequence, or an **implausible** SR mask difference vs the same part on CS1. **Follow-up log (same §):** **Soft-SPI** on **CS0** now prints **`SR2=02 SR3=21`** (matches **CS1**); **`postErase transactional SR2`/`SR3`** on **HW SPI** is **`00`/`00` on CS0** vs **`02`/`21` on CS1** — the **die / SR image is correct**; **`spi_write_read_blocking` for `35h`/`15h` on CS0 is bogus** while **`05h` SR1** can still read **`00`** (idle). Earlier **read sweep** had **CS0 `SR3(read)=FF`** via HW — also wrong vs **`21`** — so **CS0 HW `15h` is unreliable** (`FF` vs `00` vs **`21`** truth from soft-SPI). **`flash_si_test`** prints **`HW SPI baseline 500kHz SR2/SR3 (no erase)`** before the xfer loop; **observed:** **`CS0=00/00 CS1=02/21`** — **CS0** HW **`35h`/`15h`** is wrong **before** the first xfer-loop **`DCh`** (not erase-induced; post-erase matches baseline).

**Decision matrix after running the rebuilt UF2:**
- **Confirmed (2026-05 log):** Soft-SPI **CS0** **`SR2`/`SR3`** = **`02`/`21`** = **CS1**; **HW SPI** **`35h`/`15h`** on **CS0** returns **`00`/`00`** after erase (and read sweep had **`SR3=FF`** on CS0) → **RP2350 HW SPI + CS0 analogue path** for **MISO capture on multi-byte status / config reads**, not wrong chip family, not Alliance-only. **Baseline before `DCh`:** **`CS0=00/00 CS1=02/21`** — failure is **not** erase-induced.
- Soft-SPI **CS0** **`SR2`/`SR3`** also **`00`** while **CS1** shows **`02`/`21`** → **same instruction, same GPIO bit-bang**, different result per **`/CS`** → **CS0-specific analogue path** (device, socket, **`/CS0`**, or a **stray coupling** that only affects that chip’s MISO timing).
- Soft-SPI CS0 JEDEC = `FF FF FF` or `00 00 00` (CS1 fine) → **CS0 net / select** broken (superseded if lab already shows good JEDEC on CS0).
- **`heldBurst12` SR1 = `00…` on both CS** → **normal idle**; rely on **`SR2`/`SR3`** and **probe** columns, not SR1 burst alone.
- **`probe_nonff=64` on CS0** with soft tail **all `FF`** before XMODEM → first HW row **mutates** that window or **readback** is wrong; **`postErase SR2`/`SR3`** line narrows **register read** vs **array read**.

**What we did not do:** Integrate this test into `megaflash` runtime or the Applesoft validator; no change to default `SPI_SPEED_FINAL` based on JEDEC (policy remains: operator/hardware fix or future adaptive tuning).

**References:** `MegaFlash/datasheets/W25Q512JV SPI RevB 06252019 KMS.pdf`, `MegaFlash/datasheets/AlliacheFlashDatasheet.pdf` (Alliance **AS25F3512MQ**), `pico/flash.c` (`InitSpi`, `SPI_SPEED_FINAL`, `tsReadOneBlock`, `tsWriteOneBlockWithoutErase`, `ReadFromFlashByDMA`, `BITINVERSION`), `pico/mediaaccess.c` (`WriteBlockForImageTransfer`), `pico/flash_si_test/main.c`, `pico/flash_si_test/xmodem_like.c`, `pico/flash_si_test/flash_si_softspi.c`, `pico/flash_si_test/flash_si_pins.c`, `pico/dmamemops.c`, `pico/CMakeLists.txt` (`MEGAFLASH_BUILD_FLASH_SI_TEST`).

---

## 24. MegaFlash logo → Apple IIc HGR / DHGR / DLGR assets (2026-07-12)

**What:** Render the attached MegaFlash logo for Apple IIc display modes: **Hi-Res (280×192, 6 artifact colors)**, **Double Hi-Res (560×192 / 140×192 color cells, 16 colors)**, and **Double Low-Res (80×48, 16 colors)**.

**Why:** Modern vector/PNG art must be remapped to real IIc constraints. HGR cannot freely mix Green/Violet with Orange/Blue in the same 7-pixel byte (high bit selects the set). DHGR’s useful color width is **140** cells, not 560 independent chroma samples. DLGR at 80×48 needs aggressive simplification so the badge silhouette remains readable.

**What we did:**
- Keyed out the near-white studio backdrop, fitted the badge on black, then quantized with Bayer dither where gradients matter.
- HGR: per-byte palette-set choice (GV vs OB), plus an approximate **8192-byte** screen dump (`megaflash_hgr.bin`) for `$2000`.
- DHGR: quantize at **140×192**, nearest-neighbor stretch to **560×192** for previews; keep a `.idx` color-cell map.
- DLGR: BOX downsample to **80×48**, 16-color quantize; packed nibbles + `.idx` for a future poke routine.
- Assets and regenerator live in `pico/assets/apple2-logo/` (`render_apple2_logo.py`, `README.md`).

**What we did not do:** Full DHGR main/aux interleaved memory image; hand-pixel art pass for DLGR; load/display path on a connected emulator (none connected during this session).

**Takeaway:** Previews are authoritative for “how it looks under each palette.” Binary HGR is loadable; DLGR/DHGR still need a small 6502 blitter for soft-switch + memory layout.

**References:** `pico/assets/apple2-logo/`, `render_apple2_logo.py`, `megaflash_apple2_modes_sheet.png`.

---

## 25. Hardware bring-up `[hwdiag]` — REMOVED (2026-09-03)

A Debug-only 1 Hz PHI0/nDEVSEL/cycle-counter report (`pico/hwdiag.c`) was added by a concurrent
session (§1dd) to bring up newly assembled boards the IIc could not see. **Removed at the
operator's request** once it was no longer critical: `pico/hwdiag.{c,h}` deleted and the hooks
backed out of `busloop.c`, `busloop_wa.c`, `slinky.c`, `main.c`, `CMakeLists.txt`, and
`docs/Debug-mode.md`. Recorded here only so the §1dd collision notes still resolve.

**Takeaway worth keeping:** for a board the Apple never sees, `cyc=0` with `phi0` toggling points
at the GAL not selecting the Pico (A4–A15 / GP20) rather than the level shifters; `cmd10>0` while
the Apple still reports not-found points at the return data path.

*This document reflects reasoning and changes made during development; it may be extended as further design decisions are documented.*

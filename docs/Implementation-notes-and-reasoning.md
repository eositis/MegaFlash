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
| ADTProETH timeout vs captures | §**1aw** | **`rx_noroom`** = §**1au** **drops** (W5100-sized RX full); not fixed by bigger fake buffer; **`tcpdump`** UDP/6502 OK; UART **460800** |
| UART vs “Device not found” | §1c, `debug/*.log` | `w5100.s` `init` only `SEC`s on RTR XOR; correct RTR reads ⇒ that run passed Ethernet init; **`ip65_init` then `clc`s unconditionally** — see §1c if UI still says device not found; **48× `DATA read`** after `mode=0x03` traces RMSR/SHAR/OPEN |
| UART boot identity | `main.c`, `build_id.h.in`, `CMakeLists.txt` | After reboot, scroll to **latest** `Megaflash DEBUG Firmware…` block: includes **`FIRMWAREVERSTR`** and **`Firmware build:`** (UTC + Unix s from CMake **`–DFIRMWARE_BUILD_TIMESTAMP`**). **`./build-debug.sh`** refreshes those vars each configure so the UF2 matches the UART line; **`cmake --build` alone** can leave a stale **`build_id.h`**. This stamp is **configure-time wall clock**, not the git commit author date — correlate binaries with the **build script run**, not only **`git log`**. |
| ip65 init bisect (Pico 2 W Debug) | `CMakeLists.txt` `U2_IP65_CHECKPOINT`, `uthernet2.c`, `u2_monitor.c` | Default **quiet**: one **`[u2] ck=n`** per run when **`U2_IP65_CHECKPOINT=n`** (1=MODE 0x03 … 5=MACRAW OPEN). Optional **`U2_IP65_TRACE_DATA`**, **`U2_MON_LOG_BUS`** (floods UART) |
| ADTPro crash triage (`system-$01`) | §1p, `debug/uart_log.txt` | If no `sock0 OPEN/SEND/RECV` or checkpoint lines appear in failure window, treat as pre-W5100 crash/handoff issue first; use `U2_IP65_CHECKPOINT=1` build to confirm first mode write is reached |
| Apple readback for U2 | `busloop.c` U2 branch, `a2bus.h` | **`registers.r[4..7]`** → **`i32[1]`** chunk **1** → **`UpdateMegaFlashRegisters(1,…)`**; RP2350 waits for IRQ0 before update (§1d) so SM1 presents the merged byte on the next cycle |
| U2 read data path | `busloop.c`, `a2bus.h` | U2 read byte must update **chunk 1** (SM1), not chunk 0 |
| RP2350 U2 vs PIO IRQ 0 | `busloop.c` §1d | U2 branch must wait for IRQ 0 clear before `UpdateMegaFlashRegisters(1,…)` (same as main loop); skipping caused bad C0C4–C0C7 reads |
| U2 `[u2m]` monitor | `u2_monitor.c`, `build-debug.sh` §1e | Debug-only queued UART trace; flush from `U2_Poll`; bus + socket + net hooks |
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
| Uthernet II lwIP | `uthernet2_net.{h,cpp}`, `uthernet2.c`, `main.c` | **`u2_netif_input_wrapper`**: MACRAW copy **+** always **`u2_saved_netif_input`** (same as **`483e8da`**); §**1ar**. Hook install deferred via **`u2_macraw_sta_input_hook_ensure`** in **`U2_Net_Poll`** (§**1av**) — not **`OpenMacraw`** alone (CYW43 / core ordering). `U2_Net_Poll` before FIFO wait. Pico 2 W: MACRAW TX queue **16**, TCP **`Sn_TX_RD`** partial accept. Reset-abort §1ah; §1k/§1l. |
| ADTPro socket-pointer tracing | `uthernet2.c`, `u2_monitor.{h,c}` | Added `sock ptrs` monitor snapshots for `send-pre/send-post/recv-pre` with `TX_RD/TX_WR/RX_RD/RX_WR/SR`; later added coherent `RX_RD` reads (high/low/high retry) to avoid torn cross-core pointer values in `RX_RSR` and trace output (§1ai) |
| Telnet65 walkthrough + fixes | §10f, `uthernet2.c`, `uthernet2_net.cpp`, `network_pump.cpp` | Implemented: TX wrap fix (`<0`), UDP/TCP chained pbuf flattening (`pbuf_copy_partial`), removed duplicate lwIP lock in `U2_Net_OpenUdp`; later `wget65` follow-ups changed RECV to preserve unread bytes (§10h) |
| Other ip65 tools walkthrough | §10g, `ip65/apps/*`, `ip65/apps/w5100.c`, `uthernet2*.{c,cpp}` | Risks: RECV discards unread bytes (shared-access mismatch), TCP RX overflow can drop+ACK, SEND chunk currently capped at 2048; `wget65` highest risk |
| `wget65` priority fixes | §10h, `uthernet2.c`, `ip65/apps/w5100.c` | SN_CR=RECV no longer forces `RX_RD->WR`; TCP SEND drains entire queued TX data in chunks (not single 2 KiB cap) |
| TCP RX backpressure | §10i, `uthernet2_net.h`, `uthernet2.c`, `uthernet2_net.cpp` | U2 RX callback returns accepted bytes; TCP `tcp_recved()` now acknowledges only accepted payload (UDP unchanged/all-or-drop) |
| Open C0C4 unresolved item | §12 | Slot decode confirmed working; remaining investigation is nDEVSEL signal/timing visibility at Pico/PIO point vs timing/FIFO/CPU-drain behavior |
| Pump TCP + session timers | `network_pump.{h,cpp}` | `CreateTcpPcb`: `tcp_arg(owner)`, `NetworkPump_LegacyTcpRecv` / `NetworkPump_LegacyTcpErr` → `OnTcpRecvPbuf` / `OnTcpErr`; `tcp_pcb_owners_` for unregister. `ScheduleTimer` / `CancelTimer`; `PollOnce` → `DrainSessionTimers` → `OnTimer` (§14.12) |
| Reset-abort scope during U2 transfers | `main.c`, `network.{h,cpp}`, `network_pump.h` | Confirmed `nRESET` abort now requires active legacy operation; ignore abort during Uthernet-only sessions so ADTPro MACRAW transfer is not globally torn down (§1aj) |
| ADTPro mid-transfer stall (May 2026 capture) | §1ak, `debug/tcpdump.txt` | Send path (Mac→IIc) confirmed by operator; pcap src/dst for large UDP must be reconciled with Mac/IIc IPs; ~721 ms gap after shortened large payload (501→333 B); ~25× ~500 B large payloads before anomaly |
| Wire vs MACRAW **length** / **host** correlation | §1ap, `debug/tcpdump.txt`, UART **`MACRAW host`** | Ingress study used **`MACRAW rx len`** (removed). **Debug** UART: **`MACRAW host eth_len`** at **RECV** (+ **fnv**, sip→dip) vs **`tcpdump`** `length N`; **fnv** = FNV‑1a over Ethernet bytes in ring |
| ADTPro Send RC / remediation procedure | §1am, §1aq, `[u2macraw-rx]` / `[u2macraw-stage]` UART | Ring parity in **`into_ring`**; staging non-zero if FIFO used; **`reject_no_room`** only if staging also full |
| P0-3 RX cross-core | §1al, `uthernet2.c`, `Uthernet-II-stack-architecture-and-todos.md` | `_Atomic` `sn_rx_rd`/`sn_rx_wr`; single **release** publish of `sn_rx_wr` after each UDP/TCP/MACRAW enqueue; **acquire** loads in `get_rx_rsr`; **UDP/TCP** one reload if ring appears full / TCP starved |
| Inbound parity implementation (2026-05) | §1az, `uthernet2.c`, `u2_monitor.{h,c}`, `CMakeLists.txt`, `build-debug-both.sh` | W5100 drop-new MACRAW when full; **`sn_rx_wr`** remap on **`RMSR`** (not zero); **`Sn_TX_FSR`** safe when **`transmit_size==0`**; optional **`U2_MACRAW_COMPAT_DROP_OLDEST`**; **`U2_MonNetRxDrop`** rate-limited; atomic **`sn_rx_wr`** + sizing helper + `[u2m]` RX-drop telemetry |
| U2 stack architecture + TODO / validation | `docs/Uthernet-II-stack-architecture-and-todos.md` | Mermaid diagrams, contract table, P0–P2 issues, validation checklists for stabilization work |

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

*This document reflects reasoning and changes made during development; it may be extended as further design decisions are documented.*

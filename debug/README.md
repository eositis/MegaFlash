# Debug captures (MegaFlash + ip65)

## Timestamped UART logs

Save Pico **Debug** firmware serial captures here (e.g. from CoolTerm / serial monitor). Use a filename that includes **date and time** so traces stay ordered, e.g. `2026-03-21 21-42-20 FT232R USB UART #1.log`.

- **`[u2]`** — gated W5100 diagnostics (`UTHERNET2_DEBUG`). Default **low noise**: **`[u2] ck=n`** only when **`U2_IP65_CHECKPOINT=n`** (CMake cache). **Bisect** ip65 init by reconfiguring:  
  `cmake -B pico/pico2_debug -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug -DU2_IP65_CHECKPOINT=3`  
  Checkpoints: **1** = MR `0x03`, **2** = RTR `$0017`, **3** = RTR `$0018`, **4** = RMSR `$001A`, **5** = MACRAW OPEN ok. Optional: **`-DU2_IP65_TRACE_DATA=1`** (48 DATA reads), **`-DU2_MON_LOG_BUS=1`** (verbose `[u2m]` per cycle — floods UART).  
  **Flush:** `U2_MonPollFlush` runs on **core 0** (next to `NetworkPump_PollOnce`), not on the Apple bus core — avoids **cyw43 `async_context` PANIC** with **`[u2] ck=5`** (see §1g in Implementation notes).
- **`[u2m]`** — activity monitor (socket + net); per–bus-cycle lines only if **`U2_MON_LOG_BUS=1`**.

These lines come from the **Pico only**. They show that the Apple accessed the emulated Uthernet II ports, not text printed by **telnet65** or other **Apple II** programs (those go to the II’s screen or their own I/O).

## ip65 for MegaFlash (slot 4)

This folder includes a **fresh build** of the Apple II disk and zip:

| File | Purpose |
|------|---------|
| **`ip65.dsk`** | ProDOS disk with **TELNET65**, DATE65, HFS65, etc. |
| **`ip65-apple2.zip`** | Headers + libraries + same `.dsk` for developers |

**Ethernet default slot is 4** (matches MegaFlash Uthernet II at `$C0C4–$C0C7` when the card is in slot 4). Sources: `ip65/drivers/a2init.s` (`eth_init_default = 4`), `ip65/inc/ip65.h` (`ETH_INIT_DEFAULT` for `__APPLE2__`).

Rebuild from the **ip65** repo (sibling of MegaFlash):

```bash
cd /path/to/ip65
rm -f ip65.dsk ip65-apple2.zip ip65.h *.lib
make apple2
cp ip65.dsk ip65-apple2.zip /path/to/MegaFlash/debug/
```

If **telnet65** reports “Device not found” but the **tail** shows **correct RTR** (`$0017`/`$0018`), see `docs/Implementation-notes-and-reasoning.md` §1c (stock ip65 cannot return device failure from `ip65_init` after a passing RTR XOR). Otherwise see §1c–§1d (probe, RP2350 PIO IRQ wait). A single `[u2] mode=0x03` line then silence often means the Apple stopped before DATA reads.

## Manual W5100 checks after RTR (Apple monitor, slot 4)

All addresses are **`$C0C4`–`$C0C7`**: **MR**, **AddrHi**, **AddrLo**, **Data**. Start each group with **`C0C4:03`** (indirect + auto-increment). Set the pointer with **`C0C5`/`C0C6`**, then read **`C0C7`** (repeat for consecutive bytes with AI).

| Step | Pointer (hex) | What to read / expect (MegaFlash defaults) |
|------|----------------|-------------------------------------------|
| 1 | `$001A` | **RMSR** → **`06`** (ip65 short path; otherwise it does a full chip reset in software) |
| 2 | `$001B` | **TMSR** → **`06`** |
| 3 | `$0009`–`000E` | **SHAR** (6 bytes) → **`00 08 DC A2 A2 A2`** (same as ip65 `w5100.s`) |
| 4 | `$0403` | Socket 0 **S0_SR** → **`00`** (closed) before driver **OPEN** |
| 5 | (optional) | After **telnet65** has run init, **`$0403`** → **`42`** (**`W5100_SN_SR_MACRAW`**) if **OPEN** succeeded |

Steps 1–3 mirror **`w5100.s`** after the RTR XOR. Step 4 confirms socket 0 is idle. Step 5 is only meaningful **after** a successful **`eth_init`** (e.g. run telnet65 once, drop back to monitor — SR may still show MACRAW if the stack left the socket open).

**Pico UART bisect:** Rebuild Debug with **`-DU2_IP65_CHECKPOINT=n`** (`n` = 1…5). If you see **`ck=4`** but not **`ck=5`**, the Apple reached **RMSR** but not a successful **MACRAW OPEN** (see `docs/ip65-Uthernet-II-integration.md` §3).

**If the monitor passes 1–3 but telnet65 still prints “Device not found”:** That string is **`eth_init` / `ip65_init` failure** only. Use UART checkpoints to see whether the 6502 actually reaches **`ck=5`**. If **`ck=5`** appears, the Ethernet driver init **succeeded** and the message is likely a **different build**, **stale screen**, or a **non-stock** binary — not the RTR probe.

**If telnet65 fails later** (blank screen, timeout, DHCP error): that is usually **not** “device not found” — check **WiFi**, **DHCP**, and **Pico ↔ Apple** with a **Debug** UART capture while reproducing.

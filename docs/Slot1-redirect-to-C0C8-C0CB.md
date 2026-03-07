# Redirecting Slot 1 (Serial Port) to $C0C8–$C0CB

This note addresses whether it is realistic to make **PR#1** and **IN#1** use new hardware registers at **$C0C8–$C0CB** on the MegaFlash (slot 4) instead of the built-in ACIA at $C098–$C09B.

---

## Short answers

1. **Is it realistic?** **Yes**, but it needs changes in **both** the **Pico firmware** and the **Apple ROM firmware** (patches).
2. **Would PR#1 and IN#1 then use these ports?** **Yes**, provided the ROM’s slot 1 (serial port 1) driver is redirected to use $C0C8–$C0CB instead of $C098–$C09B.

---

## Current use of $C0C8–$C0CB

In the current Pico **bus loop**:

- **addr 0–3** ($C0C0–$C0C3): MegaFlash (command, param, data, ID).
- **addr 4–7 only** ($C0C4–$C0C7): Uthernet II (W5100). The condition is `addr >= U2_C0X_OFFSET && addr <= U2_C0X_LAST` (4–7).
- **addr 8–15** ($C0C8–$C0CF): **Not** Uthernet II; they fall through to MegaFlash handling (e.g. registers.r[addr] for writes, or no U2 response for reads).

So **$C0C8–$C0CB are free** for another device (e.g. serial 1 emulation). The Pico only needs to add decode and logic for addr 8–11.

---

## What has to change

### 1. Pico firmware (bus loop + new “serial 1” emulation)

- **Address decode:** Treat **addr 8–11** ($C0C8–$C0CB) as a new device. Uthernet II is already limited to addr 4–7 ($C0C4–$C0C7); addr 8–11 are free for serial/ACIA emulation.
- **New “ACIA” at $C0C8–$C0CB:** Implement four registers in an ACIA-compatible way, e.g.:
  - $C0C8 – data (read = receive, write = transmit)
  - $C0C9 – status (read) / reset (write)
  - $C0CA – command (write)
  - $C0CB – control (write, e.g. baud/format)

  The Pico would buffer bytes to/from USB serial (or an internal queue) so that reads/writes at these addresses behave like a 6551 ACIA.

With that in place, **any** code that uses $C0C8–$C0CB would talk to this emulated serial port.

### 2. Apple ROM firmware (slot 1 → $C0C8–$C0CB)

**PR#1** and **IN#1** only change the **current output/input slot**; the actual I/O is done by the **slot 1 driver** in the ROM, which today uses the ACIA at **$C098–$C09B**. So for PR#1/IN#1 to use the MegaFlash serial:

- The ROM’s **slot 1 serial driver** must be made to use **$C0C8–$C0CB** instead of $C098–$C09B.

Two main approaches:

**Option A – Patch every ACIA reference**

- Find all places in the IIc ROM (e.g. in the monitor/serial driver) that load or store to $C098, $C099, $C09A, $C09B.
- Replace those addresses with $C0C8, $C0C9, $C0CA, $C0CB (or with a small stub that uses these addresses).
- Pros: No new code; minimal new segments.  
- Cons: Many patches; any mistake or missed reference can break serial 1; ROM version–dependent.

**Option B – New driver + vector redirect**

- Add a **new driver** in the firmware (in a segment that fits in the ROM) that implements “serial port 1” using **only** $C0C8–$C0CB (read/write/status/init).
- Find in the ROM where the **slot 1 output/input vectors** (or the single “slot 1 handler”) are set or called when PR#1/IN#1 are used.
- Patch so that COUT/GET (or the equivalent) for slot 1 call **our** driver instead of the original ACIA code.
- Pros: One clear boundary; original ACIA code untouched; easier to maintain.  
- Cons: Requires locating the slot 1 vector/entry in the ROM (e.g. from disassembly or technical docs).

So: **yes, it’s realistic**, but it requires both Pico changes (decode 8–11, implement ACIA-like registers) and ROM patches (redirect slot 1 I/O to $C0C8–$C0CB).

---

## PR#1 and IN#1

- **PR#1** sets the **current output** device to “slot 1”. Subsequent COUT (e.g. PRINT, LIST) go through the slot 1 driver.
- **IN#1** sets the **current input** device to “slot 1”. Subsequent GET/INPUT read from the slot 1 driver.

If the **slot 1 driver** in the ROM is the only code that performs the actual reads/writes for that slot, and we change that driver to use **$C0C8–$C0CB** (by either Option A or B above), then **PR#1 and IN#1 will indeed use the MegaFlash serial ports** at $C0C8–$C0CB. No change to how PR#1/IN#1 are invoked; only the addresses the driver uses change.

---

## Summary

| Question | Answer |
|----------|--------|
| Redirect slot 1 to $C0C8–$C0CB? | **Realistic** with Pico decode + ACIA emulation and ROM patches. |
| PR#1 / IN#1 use $C0C8–$C0CB? | **Yes**, once the ROM’s slot 1 driver is redirected to those addresses. |
| Current $C0C8–$C0CB? | **Free.** Uthernet II is limited to $C0C4–$C0C7; addr 8–11 fall through to MegaFlash (no U2). Ready for serial 1 or other use. |

So: **yes**, it is realistic to change the slot 1 code to point at new hardware at $C0C8–$C0CB, and **yes**, using PR#1 and IN#1 would then target those ports on the MegaFlash, **provided** you (1) implement an ACIA-compatible device at $C0C8–$C0CB on the Pico and (2) patch the IIc ROM so the slot 1 driver uses $C0C8–$C0CB instead of $C098–$C09B.

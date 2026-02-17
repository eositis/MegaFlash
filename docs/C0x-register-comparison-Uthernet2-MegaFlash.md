# C0x Register Comparison: Uthernet II vs MegaFlash

Both the **Uthernet II** (AppleWin emulation) and **MegaFlash** (hardware) use the same Apple II slot I/O region and decode the same **low 2 address bits** for their first four registers. So from the 6502’s point of view, the **first four C0x locations have the same address layout**; the meaning of each register differs.

---

## 1. Address decoding

| Aspect | Uthernet II (AppleWin) | MegaFlash (Pico) |
|--------|------------------------|------------------|
| **Slot I/O range** | One slot, C0xx page (e.g. C0s0–C0sF) | Slot 4: $C0C0–$C0CF |
| **Bits decoded** | **A0, A1 only** (`address & 0x03`) | **A0–A3** (16 registers); first 4 use A1:A0 like U2 |
| **Registers** | 4 locations (mode, addr high, addr low, data) | 16 locations; first 4 = CMD/Status, Param, Data, ID |
| **Defines** | `W5100.h`: `U2_C0X_MASK 0x03`, then `(0x04&3)`..`(0x07&3)` | `defines.h`: CMDREG=0, PARAMREG=1, DATAREG=2, IDREG=3 |

So:

- **Uthernet II**: `loc = address & U2_C0X_MASK` → **4 ports**, indices 0–3.
- **MegaFlash**: First 4 registers use the same (addr & 3) → indices 0–3; the rest use A2:A0 or A3–A0 for 16 total.

---

## 2. First four registers: same addresses, different roles

Both use the same **address mapping** for the first four soft switches:

| (addr & 3) | Uthernet II (W5100) | MegaFlash |
|------------|----------------------|-----------|
| **0** | Mode register (MR) — R/W; chip mode, reset, address auto-increment | CMD (write) / STATUS (read) — command and status |
| **1** | Address high byte — set 16-bit indirect address | PARAM — parameter byte |
| **2** | Address low byte — set 16-bit indirect address | DATA — data byte |
| **3** | Data port — R/W byte at current address (optional auto-increment) | ID — device ID / extended register |

So the **C0x register layout for the first four locations is the same**; the **semantics** differ.

---

## 3. Similarities

1. **Same slot I/O model**  
   One 16-byte block per slot in the C0xx page.

2. **Same decoding for the first 4 locations**  
   Only the bottom 2 bits of the address select the register (0–3). So e.g. $C0C0/$C0C4/$C0C8/$C0CC all map to reg 0 on MegaFlash when A2–A3 are ignored; Uthernet II only ever decodes 2 bits, so it has exactly 4 ports.

3. **Register 0 = control**  
   U2: mode (MR); MF: command/status.

4. **Registers 1 and 2 = “second and third” port**  
   U2: high/low byte of indirect address; MF: param and data.

5. **Register 3 = “fourth” port**  
   U2: data port (indirect W5100 access); MF: ID (and in Slinky emulation, address auto-increment on read of this register).

So **same C0x register layout (first 4), different device behavior**.

---

## 4. Differences

| Item | Uthernet II | MegaFlash |
|------|-------------|-----------|
| **Access model** | **Indirect**: set 16-bit address in reg 1+2, then read/write bytes at reg 3 (W5100 memory/registers). Reg 0 = chip mode (e.g. auto-increment). | **Direct**: four (or 16) separate registers; no address register. Reg 0 = command/status, 1 = param, 2 = data, 3 = ID. |
| **Number of ports** | 4 only (A0, A1). | 16 (A0–A3); first 4 match U2 layout. |
| **Hardware** | Emulated W5100 (Uthernet II) in AppleWin. | Real Pico (RP2040/RP2350); PIO drives A0–A3 (and data/RnW). |
| **Reg 0** | Mode register (reset, AI bit, etc.). | Command (write) / Status (read). |
| **Reg 1–2** | Single 16-bit address for indirect access. | Separate Param and Data bytes. |
| **Reg 3** | Data port for W5100; one byte per read/write at current address. | ID register; in Slinky mode, reading reg 3 auto-increments internal address (similar idea to “data port,” but for RAM buffer). |

---

## 5. Code references

**Uthernet II (AppleWin)**  
- `source/W5100.h`: `U2_C0X_MASK 0x03`, `U2_C0X_MODE_REGISTER` … `U2_C0X_DATA_PORT` from `(0x04&3)` … `(0x07&3)`.  
- `source/Uthernet2.cpp`: `IO_C0()` uses `loc = address & U2_C0X_MASK` and switches on `loc` 0–3 for mode, address high, address low, data.

**MegaFlash**  
- `pico/defines.h`: `CMDREG 0` ($C0C0), `PARAMREG 1`, `DATAREG 2`, `IDREG 3`.  
- `pico/a2bus_rp2040.pio` / `a2bus_rp2350.pio`: “16 I/O registers from $C0C0 to $C0CF”; A3–A0; “A2 and A3 must be pulled down to set the address at $C0C0–C0C3” (so first 4 = same as U2 decoding).  
- `pico/slinky.c`: uses `addr = busdata & 0b0011` (ignore A3–A2), so Slinky uses the same 4 locations as Uthernet II.

---

## 6. Summary

- **Same C0x API for the first 4 registers**: both use `(address & 3)` to select one of four 8-bit registers at the same logical addresses in the slot’s C0xx block.  
- **Uthernet II**: those 4 ports implement **indirect W5100 access** (mode, address high, address low, data).  
- **MegaFlash**: those 4 ports are **direct** CMD/Status, Param, Data, ID; it can also decode A2–A3 to support 12 more registers ($C0C4–$C0CF).

So firmware that only uses the first four C0x locations can treat the **addressing** as identical; only the **semantics** of each register differ between the two cards.

---

## 7. Register 0 (address 0): syntax overlap and merge

### 7.1 What each project does at address 0

| Aspect | Uthernet II | MegaFlash |
|--------|-------------|-----------|
| **Address** | First C0x location `(addr & 3) == 0` | Same: CMDREG / STATUSREG at index 0 |
| **Read** | Returns **Mode Register** (same byte as last written) | Returns **Status** (busy flag, error flag, error code) |
| **Write** | Writes **Mode Register** (MR_IND, MR_AI, MR_PPOE, MR_PB, MR_RST) | Writes **Command** (CMD_* 0x00–0x53+) |
| **R/W symmetry** | **Symmetric**: read = last written value | **Asymmetric**: write = command, read = status |

So there **is** address overlap (same register index 0), but **no** syntax overlap: U2 uses one R/W mode byte; MegaFlash uses write‑as‑command / read‑as‑status at the same address.

### 7.2 Value overlap (writes to address 0)

Same byte written to reg 0 is interpreted differently:

| Value | Uthernet II (Mode Register) | MegaFlash (Command) |
|-------|-----------------------------|----------------------|
| 0x00 | Clear MR | CMD_RESETBOTHPTRS |
| 0x01 | Set IND bit | CMD_RESETDATAPTR |
| 0x02 | Set AI (address auto-increment) | CMD_RESETPARAMPTR |
| 0x03 | Set IND \| AI | CMD_MODELINEAR |
| 0x08 | Set PPOE | CMD_LOAD_CPANEL |
| 0x10 | Set PB | CMD_GETDEVINFO |
| 0x80 | RST (chip reset) | (not a command; status read can be 0x80 = busy) |

So the **value space overlaps**: you cannot tell from the byte alone whether it was meant as W5100 mode or a MegaFlash command.

### 7.3 Can the two use the same addresses without conflict?

- **Different slots**: If Uthernet II is in one slot and MegaFlash in another, there is **no conflict**: each card only sees its own slot’s C0x space. Same address layout, different slots = fine.
- **One card that is “both”**: Not without extra rules. A single device at one slot cannot honestly implement both protocols on the same register 0, because:
  - One **write** would have to mean either “set W5100 mode” or “run MegaFlash command.”
  - One **read** would have to return either “mode register” or “status.”
  - Many **values** are valid in both (0x00, 0x01, 0x02, 0x03, 0x08, 0x10, 0x80, …), so value-based partitioning is messy.

Ways to **merge** without conflict would require a **new, unified protocol**, for example:

1. **Protocol select**  
   Use another register or a magic sequence to choose “Uthernet II mode” vs “MegaFlash mode”; then reg 0 behaves as one or the other. No overlap at runtime, but not the same as “same addresses, no conflict” for existing U2/MF software.

2. **Reserve bits in reg 0**  
   e.g. “If bit 6 set on write, treat as MegaFlash command; else treat as W5100 MR.” Then you’d reserve that bit for U2 (W5100 MR doesn’t use 0x40) and restrict MF command encoding. Doable but not backward‑compatible with existing MF command set (0x09, 0x0a, etc. would need redefinition).

3. **Keep separate slots**  
   No protocol change: U2 and MegaFlash both in slot 4; U2 at C0x4–C0x7 ($C0C4–$C0C7), MegaFlash at C0x0–C0x3 ($C0C0–$C0C3). Same slot, no address conflict.

**Summary**: There **is** address overlap at register 0 (same C0x index), but **no** compatible syntax overlap: read/write roles and value meanings differ. You **can** use the same addresses in the two projects only by keeping the cards in **different slots**. To merge both behaviors in one slot without conflict you need an explicit unified protocol (e.g. mode select or reserved bits), not “same addresses as-is.”

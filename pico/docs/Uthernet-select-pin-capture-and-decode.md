# Uthernet select: pin capture and action selector

Logic shows **nDEVSEL** goes low on **$C0C4** and address pins are **pin6=0, pin7=0, pin8=1, pin9=0**. This doc traces where those pins are captured and how the C code chooses MegaFlash vs Uthernet II.

---

## 1. Pin mapping (address bus A0–A3)

From **a2bus_rp2040.pio** / **a2bus_rp2350.pio**:

| Apple II address bit | GPIO | Your measurement |
|----------------------|------|------------------|
| A0                    | 6    | 0                |
| A1                    | 7    | 0                |
| A2                    | 8    | 1                |
| A3                    | 9    | 0                |

So the 4‑bit address nibble = **0b0100 = 4** → **$C0C4** (C0x4).

---

## 2. Where the pin state is captured (PIO)

### RP2040 (a2bus_rp2040.pio)

- **Input base:** `sm_config_set_in_pins(&c, A2BUS_BASE)` with `A2BUS_BASE = 6` → pins 6–18.
- **Width:** `A2BUS_WIDTH = 13` (A3–A0 + R/nW + 8‑bit data).
- **Shift:** `sm_config_set_in_shift(&c, false, false, 0)` → shift left = false, so **first pin (6) = LSB** of the shifted value.

Capture in the PIO program:

```asm
  in pins, A2BUS_WIDTH  ; Read 13 bits: pins 6..18
  push noblock          ; Push to this SM's FIFO
```

So the 13‑bit value has:

- **Bits 0–3:** pins 6–9 = A0–A3 (address nibble)
- **Bit 4:** pin 10 = R/nW
- **Bits 5–12:** pins 11–18 = D0–D7

All four state machines run the same program; when nDEVSEL goes low they all do this and push the **same** 13 bits to their **own** FIFO. The CPU only reads from **SM 0** (see below).

### RP2350 (a2bus_rp2350.pio)

- Same mapping: `sm_config_set_in_pins(&c, A2BUS_BASE)`, `A2BUS_BASE = 6`, `sm_config_set_in_shift(&c, false, false, 0)`.
- The **a2buslistener** program does the capture:

```asm
  wait 0 gpio nDEVSEL_GPIO   ; wait until nDEVSEL is active (low)
  jmp PIN, read_cycle        ; JMP pin = R/NW (pin 10)
  nop [16]                   ; write path delay
read_cycle:
  mov isr, null
  in pins, A2BUS_WIDTH       ; Read 13 bits: pins 6..18
  push noblock               ; Push to listener SM FIFO
```

So again: **bit 0 = pin 6 = A0**, **bit 1 = pin 7 = A1**, **bit 2 = pin 8 = A2**, **bit 3 = pin 9 = A3**.

---

## 3. How the CPU gets the captured value

**a2bus.h:**

```c
#define SM_LISTENER 0

static inline uint32_t GetAppleBusBlocking() {
  return pio_sm_get_blocking(pio0, SM_LISTENER);
}
```

- **RP2040:** Only **SM 0** is read; all four SMs push the same 13 bits when nDEVSEL goes low, so the value from SM 0 is the captured bus (address + R/nW + data).
- **RP2350:** The **listener** SM (SM 0) is the one that does `in pins` + `push`; the CPU reads that same SM, so again it gets the 13‑bit value with the same bit layout.

So in both platforms the CPU receives a **busdata** value where:

- **bits 0–3** = A0–A3 (address nibble for C0x0–C0xF)
- **bit 4** = R/nW (1 = read, 0 = write)
- **bits 5–12** = data bus D0–D7

---

## 4. Where address is derived and Uthernet is selected (C code)

**busloop.c** (main loop):

```c
    // 8-bit data from Apple + RnW Flag + 4-bit address from Apple
    uint32_t busdata = GetAppleBusBlocking();
    uint32_t addr = busdata & 0b1111;     // Lower nibble = A0..A3 from pins 6–9

    /* Address decode: C0x0–C0x3 = MegaFlash; C0x4–C0x7 = Uthernet II; ... */
    if (addr >= U2_C0X_OFFSET && addr <= U2_C0X_LAST) {
      // Uthernet II: $C0C4–$C0C7 (addr 4–7)
      uint8_t u2_read_byte;
      U2_HandleBusAccess(busdata, &u2_read_byte);
      if (busdata & READFLAG) {
        registers.r[addr] = u2_read_byte;
        UpdateMegaFlashRegisters(1, registers.i32[1]);
      }
      // ...
      continue;
    }

    if (busdata & READFLAG) {
      switch(addr) {
        case DATAREG:  // 2
        case PARAMREG: // 1
        case IDREG:    // 3
        // ... MegaFlash $C0C0–$C0C3
      }
    } else {
      switch(addr) {
        case CMDREG:   // 0
        case DATAREG:  // 2
        case PARAMREG: // 1
        case IDREG:    // 3
        // ... MegaFlash $C0C0–$C0C3
        default:
          registers.r[addr] = data;  // $C0C4–$C0CF as RAM
      }
    }
```

**defines.h:**

```c
#define U2_C0X_OFFSET  4   /* first Uthernet II address ($C0C4) */
#define U2_C0X_LAST    7   /* last Uthernet II address ($C0C7) */
```

So:

- **addr = busdata & 0b1111** = state of pins 6,7,8,9 (A0–A3).
- With **pin6=0, pin7=0, pin8=1, pin9=0** → **addr = 4**.
- **addr >= 4 && addr <= 7** → Uthernet II branch runs (`U2_HandleBusAccess`), which is correct for **$C0C4**.

---

## 5. Summary

| Step | Where | What |
|------|--------|------|
| Capture | PIO `in pins, 13` from base pin 6 | Pin 6→bit0, 7→bit1, 8→bit2, 9→bit3 (A0–A3) |
| To CPU | `GetAppleBusBlocking()` → `pio_sm_get_blocking(pio0, SM_LISTENER)` | 13‑bit value, low 4 bits = address |
| Address | `addr = busdata & 0b1111` | 4 for your measurement (0b0100) |
| Selector | `addr >= U2_C0X_OFFSET && addr <= U2_C0X_LAST` (4–7) | True for $C0C4 → Uthernet II path |

So with **pin6=0, pin7=0, pin8=1, pin9=0** and nDEVSEL low on $C0C4, the firmware **should** interpret the access as **addr=4** and run the Uthernet II handler. If something else is happening at runtime, the next place to check is that the CPU is actually receiving this 13‑bit value (e.g. no other code draining the listener FIFO or using a different SM).

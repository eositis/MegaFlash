# Uthernet II Emulation on MegaFlash

This document describes the Uthernet II (W5100) emulation implemented in the MegaFlash firmware. Slot selection uses the **default MegaFlash bus behaviour** (no GPIO slot-select pin); the card responds when the bus is active for its slot.

---

## 1. Overview

- **Purpose:** Emulate a Uthernet II Ethernet card in slot 4 so that W5100-based Apple II software (TCP/UDP) can use the Pico’s network (lwIP over CYW43 WiFi on Pico 2 W).
- **Selector:** Address decode only: **C0x4–C0x7** ($C0C4–$C0C7) = Uthernet II; **C0x0–C0x3** ($C0C0–$C0C3) = MegaFlash. No GPIO pin is used for slot select.
- **Design reference:** AppleWin’s Uthernet2 and W5100 logic; network backend is lwIP/CYW43, not PCap/Windows.

---

## 2. Hardware

| Item | Description |
|------|-------------|
| **Slot** | Card is in **slot 4** (or hardware presents the bus when that slot is addressed). |
| **Slot select** | **None.** Same as original MegaFlash: the bus is only active when the card’s slot is addressed; no GPIO pin is used. |
| **Bus** | Same Apple II bus lines used for MegaFlash (address, data, R/W). |

---

## 3. Software Architecture

### 3.1 Files

| File | Role |
|------|------|
| `pico/uthernet2.h` | Public API: `U2_Init()`, `U2_Poll()`, `U2_HandleBusAccess(busdata, read_byte_out)`. |
| `pico/uthernet2.c` | W5100 state (8 KB RAM, mode/data address, sockets), C0x decode, read/write value, RX push into buffer, SEND (read TX buffer → network). |
| `pico/uthernet2_net.h` | Network API: init with push_rx callback, open/close/connect/listen/send, get status, poll. |
| `pico/uthernet2_net.c` | lwIP TCP/UDP: per-socket pcbs, OPEN/CONNECT/LISTEN/CLOSE/SEND, recv callbacks → push_rx. Stubbed when `PICO_CYW43_ARCH_POLL` is not set. |
| `pico/w5100_regs.h` | W5100/Uthernet II constants: C0x mask, MR/GAR/SUBR/SHAR/SIPR, socket regs, SN_MR/SN_CR/SN_SR, etc. |
| `pico/defines.h` | `U2_C0X_OFFSET` (4). No GPIO slot select. |
| `pico/main.c` | `U2_Init()` only (no pin 27 or other slot-select init). |
| `pico/busloop.c` | When addr ≥ 4: call `U2_HandleBusAccess`, merge read byte into chunk 0 and `UpdateMegaFlashRegisters(0, …)`, then every 500 cycles call `U2_Poll()`. C0x0–C0x3 = MegaFlash; C0x4–C0x7 = Uthernet II. |

### 3.2 Data Flow

- **C0x:** Only A0/A1 are decoded → four “registers”: mode (C0x0), address high (C0x1), address low (C0x2), data port (C0x3). Indirect access to full W5100 space (mode, address, then read/write data with optional auto-increment).
- **Reads:** U2 returns the byte for the selected C0x register; the bus loop merges it into the PIO chunk so the Apple sees the correct value.
- **Network:** lwIP runs inside `cyw43_arch_lwip_begin/end`. `U2_Poll()` calls `cyw43_arch_poll()` so TCP/UDP progress and recv callbacks run; received data is pushed into the W5100 RX buffers via `u2_push_rx`.

---

## 4. C0x and W5100 Register Interface

### 4.1 C0x Layout (A0/A1 only)

| C0x | Address bits | Read | Write |
|-----|--------------|------|-------|
| 0 | A1=0, A0=0 | Mode register (MR) | Mode register (bit 7 = RST resets chip) |
| 1 | A1=0, A0=1 | Address high byte | Address high byte |
| 2 | A1=1, A0=0 | Address low byte | Address low byte |
| 3 | A1=1, A0=1 | Data port (read value at current address; auto-increment if MR bit 1 set) | Data port (write value; auto-increment if MR bit 1 set) |

So: set address with C0x1/C0x2, then read/write bytes via C0x3. MR bit 1 = **AI** (auto-increment address after data access).

### 4.2 W5100 Memory Map (indirect via data port)

- **0x0000:** MR (mode), RST in bit 7.
- **0x0001–0x002F:** Common registers (GAR, SUBR, SHAR, SIPR, RTR, RCR, RMSR, TMSR, PTIMER, etc.).
- **0x0400–0x07FF:** Socket 0–3 register blocks (each 0x100 bytes): SN_MR, SN_CR, SN_SR, PORT, DIPR, DPORT, TX_FSR, TX_RD/WR, RX_RSR, RX_RD, etc.
- **0x4000–0x5FFF:** TX buffers (per-socket base/size from TMSR).
- **0x6000–0x7FFF:** RX buffers (per-socket base/size from RMSR).

Total 8 KB; address wraps at 0x6000/0x8000 when auto-increment is on (per Uthernet II doc).

---

## 5. Network Stack and RX Path

### 5.1 Supported Modes

- **TCP:** OPEN → (CONNECT or LISTEN) → SEND / RECV; status SYNSENT → ESTABLISHED.
- **UDP:** OPEN → SEND / RECV; status SOCK_UDP.
- **MACRAW:** OPEN with SN_MR = MACRAW (0x04) → SEND / RECV; status SOCK_MACRAW. RX buffer format: 2-byte length (big-endian) then raw Ethernet frame. TX: host writes full frame to TX buffer and issues SEND; frame is sent via netif linkoutput. MACRAW RX can be fed via `U2_Net_FeedMacrawRx(i, data, len)` if a driver hook is available (e.g. from the CYW43 receive path).

IPRAW is **not** implemented. AppleWin-only features (e.g. Virtual DNS) are **not** implemented.

### 5.2 Socket Commands (SN_CR)

- **OPEN:** From SN_MR (protocol) and SN_PORT: open UDP with `U2_Net_OpenUdp(i, port)` or TCP with `U2_Net_OpenTcp(i)`.
- **CONNECT:** From SN_DIPR, SN_DPORT: `U2_Net_ConnectTcpEx(i, dest_ip_net, dest_port)`.
- **LISTEN:** From SN_PORT: `U2_Net_ListenTcp(i, port)`.
- **CLOSE / DISCON:** `U2_Net_Close(i)`.
- **SEND:** Read payload from W5100 TX buffer (between TX_RD and TX_WR), send via `U2_Net_SendUdp` or `U2_Net_SendTcp`; then advance TX_RD to TX_WR.
- **RECV:** RSR is computed from (sn_rx_wr − RX_RD) in the circular RX buffer; no extra action required.

### 5.3 RX Path (push into W5100 buffer)

- **UDP:** Recv callback receives (data, len, remote IP, remote port). Push into socket RX buffer: 4B IP + 2B port + 2B length (big-endian), then payload. Advance `sn_rx_wr` in the circular buffer.
- **TCP:** Recv callback receives (data, len). Push payload only; advance `sn_rx_wr`.

RSR (receive size) is **computed** when the Apple reads SN_RX_RSR0/1: `(sn_rx_wr - RX_RD + size) % size`.

### 5.4 lwIP Integration

- **Threading:** All lwIP use is inside `cyw43_arch_lwip_begin()` / `cyw43_arch_lwip_end()`. The bus loop (and thus `U2_Poll()`) runs on core 1; CYW43/lwIP are shared with other tasks (e.g. TFTP on core 0), so the arch lock serializes access.
- **Polling:** `U2_Poll()` is called every 500 Uthernet II bus cycles (addr ≥ 4) to run `cyw43_arch_poll()` and thus advance TCP/UDP and run recv callbacks.
- **Non–Pico W builds:** When `PICO_CYW43_ARCH_POLL` is not defined, `uthernet2_net.c` is stubbed (all net calls no-op, status always CLOSED).

---

## 6. Initialization and Bus Integration

1. **main.c:** `U2_Init()` (no GPIO slot select).
2. **U2_Init:** Calls `U2_Net_Init(u2_push_rx)` and then `u2_reset()` (8 KB RAM, default regs, socket base/size from RMSR/TMSR).
3. **Bus loop:** On each cycle, if address ≥ 4 (C0x4–C0x7):
   - Call `U2_HandleBusAccess(busdata, &u2_read_byte)`.
   - If read cycle: merge `u2_read_byte` into the correct byte of `registers.i32[0]` and call `UpdateMegaFlashRegisters(0, merged)`.
   - Every 500 such cycles, call `U2_Poll()`. C0x0–C0x3 are handled as MegaFlash. No GPIO slot select.

---

## 7. Usage Notes

- **WiFi:** CYW43/lwIP must be initialized (e.g. by an earlier NTP/TFTP run or by the application). If the stack is not up, OPEN will effectively leave the socket closed.
- **Slot:** The card is in slot 4; slot selection uses the default MegaFlash bus (no GPIO).
- **Buffer sizes:** Default RMSR/TMSR 0x55 gives 2 KB per socket TX/RX; same as common AppleWin setup.

---

## 8. Limitations

- **No IPRAW:** Only TCP, UDP, and MACRAW are implemented.
- **No Virtual DNS:** DNS is not part of this emulation; host resolution must be done elsewhere if needed.
- **Non–Pico W:** On builds without CYW43 (e.g. plain Pico), the network layer is stubbed and sockets remain closed.
- **Single connection per socket:** For TCP listen, one accepted connection per socket at a time; close it to listen again.

---

## 9. A2osX Compatibility

**UTHERNET2.DRV (MACRAW-only) is now supported** for socket 0 in MACRAW mode: OPEN with SN_MR = MACRAW, READ (2-byte length + frame), WRITE (frame), SEND, RECV. MACRAW TX is implemented (netif linkoutput). MACRAW RX can be used if raw frames are fed via `U2_Net_FeedMacrawRx(0, data, len)` from a driver hook.

**UTHER2.AI.DRV** still uses Socket 0 MACRAW plus Sockets 1–3 **IPRAW** (ICMP, TCP, UDP by protocol). IPRAW is **not** implemented, so UTHER2.AI remains partially incompatible (socket 0 MACRAW works; IPRAW sockets do not).

---

## 10. Reference

- **AppleWin:** `source/Uthernet2.cpp`, `source/W5100.h` (C0x layout, W5100 registers, socket behavior, RX/TX buffer layout).
- **W5100:** Datasheet memory map and socket register descriptions.
- **MegaFlash C0x comparison:** `docs/C0x-register-comparison-Uthernet2-MegaFlash.md` (how Uthernet II C0x differs from MegaFlash C0x).
- **A2osX:** `DRV/UTHERNET2.DRV.S.txt`, `DRV/UTHER2.AI.DRV.S.txt`, `INC/NIC.W5100.I.txt` (W5100 usage and MACRAW/IPRAW).

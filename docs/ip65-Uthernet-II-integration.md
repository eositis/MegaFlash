# ip65 Stack and MegaFlash Uthernet II Integration

**Goal:** Make the [ip65](https://github.com/cc65/ip65) TCP/IP stack work with MegaFlash’s Uthernet II (W5100) emulation. **No changes to ip65.** The MegaFlash Uthernet II emulation must satisfy ip65’s expectations.

---

## 1. How ip65 talks to the W5100

### 1.1 Driver chain

- **ethernet.s** – Generic wrapper; calls a **driver** table: `eth+driver::init`, `eth+driver::poll`, `eth+driver::send`.
- **w5100driver.s** – Exports `eth = _w5100` (the actual driver).
- **uthernet2.s** – Apple II Uthernet II; just ` .include "w5100.s"` and the name `"Uthernet II"`.

So the real contract is **w5100.s**: W5100 register-level driver with init/poll/send/exit and the standard driver layout (signature, MAC, bufaddr, bufsize, jump table).

### 1.2 Slot and I/O ports

- Init is called with **slot number in A**. The driver converts it to a slot I/O offset (`slot << 4`) and **fixes up** all I/O addresses at runtime.
- Base addresses (pre-fixup) are:
  - **mode**  = $C084  →  slot 4: **$C0C4**
  - **addr**  = $C085  →  slot 4: **$C0C5** (address high), **$C0C6** (address low)
  - **data**  = $C087  →  slot 4: **$C0C7**

So for slot 4, ip65 uses **$C0C4–$C0C7**, which matches MegaFlash’s U2 range (C0x4–C0x7 → Mode, AddrHigh, AddrLow, Data). **No change needed** on either side for port layout.

- ip65’s Apple II default slot can come from `ethernet.slot` (e.g. apps open it and read slot). MegaFlash U2 is fixed at **slot 4** ($C0C4–$C0C7). As long as the app uses slot 4 for Uthernet II, we’re aligned.

### 1.3 W5100 programming model (ip65)

1. **Indirect addressing**
   - Write **Mode** with Indirect + Address Auto-Increment ($03) or + Ping Block ($13).
   - Write **Address High** and **Address Low** to set the current register/buffer address.
   - Read/write **Data**; with AI, address auto-increments after each access.

2. **Init sequence (w5100.s)**
   - Soft reset if needed (Mode $80, wait until not busy, then Mode $13).
   - **Chip check:** Read RTR at 0x0017/0x0018; expect XOR = $07^$D0 (i.e. 0x07, 0xD0).
   - **RX memory size:** Read 0x001A; if not 0x06, do full reset. Then write 0x0A to 0x001A (and 0x001B) → 4 KB RX and 4 KB TX for socket 0 (and 1).
   - Write MAC to SHAR (0x0009).
   - Socket 0: Mode = **MACRAW** (0x44) + MAC filter, Command = **OPEN** (0x01).

3. **Poll (receive)**
   - Wait until Socket 0 **Command Register** (0x0401) = 0.
   - Read **Socket 0 RX Received Size** (0x26/0x27); if 0, return “no packet”.
   - Set “parameters” for read: base $6000 (Socket 0 RX), direction read.
   - Read **RX_RD** (0x28/0x29) via data port → current read pointer.
   - Physical address = $6000 + that offset (with wraparound). Read 2-byte **length header**, then payload.
   - Issue **RECV** (0x40) to Socket 0 Command Register (and update pointer shadow for next time).

4. **Send**
   - Wait until Socket 0 Command = 0.
   - Read **TX Free Size** (0x20/0x21); wait until ≥ packet length.
   - Set address to TX buffer ($4000 + TX_WR offset), write payload.
   - Issue **SEND** (0x20) to Socket 0 Command Register.

So ip65 uses **Socket 0 only**, in **MACRAW** mode: raw Ethernet frames with a 2-byte length header in the RX buffer.

---

## 2. What MegaFlash U2 already provides

- **Ports:** C0x4 = Mode, C0x5 = AddrHigh, C0x6 = AddrLow, C0x7 = Data; indirect addressing and auto-increment. **Matches ip65.**
- **Registers:** Full W5100 memory map in `u2_memory[]`; common and socket registers; MR, RTR, RMSR/TMSR, SHAR, Socket 0 MR/CR/SR, TX_FSR, TX_RD/WR, RX_RSR, RX_RD/RD. **Matches.**
- **Reset:** `u2_reset()` sets RTR = 0x07, 0xD0; RMSR/TMSR = 0x55 (default). **Chip check (0x07^0xD0) passes.** RMSR 0x06 is not the default; see below.
- **Socket 0 MACRAW:** OPEN → `U2_Net_OpenMacraw(0)`. SEND → copy from TX buffer and call `U2_Net_SendMacraw(0, buf, len)`. **Send path is there.**
- **MACRAW RX format:** `u2_push_rx_macraw()` writes 2-byte length (big-endian) then frame. **Matches ip65’s expectation.**
- **RECV command:** Currently `W5100_SN_CR_RECV` only calls `U2_Net_RecvConfirm(i)`, which is a **no-op**. So **RX_RD is never advanced** after the 6502 reads a packet. So RSR never drops to 0 and the next poll still sees the same data until a new packet overwrites it. **This must be fixed** so that RECV advances RX_RD to the current write pointer (sn_rx_wr).

---

## 3. Gaps and required adaptations (MegaFlash side only)

### 3.1 RECV must advance RX_RD (required)

**ip65:** After reading the packet, it issues RECV. The W5100 semantics are that RECV “confirms” the read and advances the read pointer so the next poll sees 0 bytes until new data arrives.

**MegaFlash:** In `write_socket_register()`, when `W5100_SN_CR_RECV` is written, advance Socket 0’s **RX_RD** in `u2_memory` to the current **sn_rx_wr** (with the socket’s receive size mask), so that `get_rx_rsr(0)` becomes 0 until the next frame is pushed.

**Concrete change (uthernet2.c):** In the `W5100_SN_CR_RECV` branch, after `U2_Net_RecvConfirm(i)`, set:

```c
u2_memory[s->register_address + W5100_SN_RX_RD0] = (uint8_t)(s->sn_rx_wr >> 8);
u2_memory[s->register_address + W5100_SN_RX_RD1] = (uint8_t)(s->sn_rx_wr);
```

(With the same mask as used in get_rx_rsr if you want strict wraparound; for “catch up” semantics, assigning sn_rx_wr is enough.)

### 3.2 RMSR/TMSR init value (optional but recommended)

**ip65:** Reads 0x1A; if not 0x06, does full reset. Then it **writes** 0x0A to 0x1A and 0x1B (4 KB per socket for 0 and 1). So it doesn’t rely on default 0x55; it programs 0x0A. MegaFlash’s `set_rx_sizes` / `set_tx_sizes` already interpret 0x0A correctly (4K+4K for first two sockets). **No change strictly required**; default 0x55 is overridden by ip65’s write. If you want to avoid the “not 0x06 → full reset” path on first init, you could set RMSR default to 0x06 in `u2_reset()`; that’s optional.

### 3.3 MACRAW receive path: feeding frames into the emulation (critical)

**ip65:** Expects to **receive** raw Ethernet frames in Socket 0’s RX buffer (2-byte length + frame). So something must call `U2_Net_FeedMacrawRx(0, data, len)` for every relevant incoming frame.

**MegaFlash today:** `U2_Net_SendMacraw()` sends via `netif->linkoutput()` (raw frame). There is **no symmetric path** for **incoming** link-layer frames. lwIP’s normal flow is: CYW43 driver receives a frame → passes to lwIP → netif input → IP reassembly and then UDP/TCP. So **IP payloads** are handled; **raw Ethernet frames** are not normally handed to an application (or to our emulation).

**Ways to feed MACRAW RX (all on MegaFlash / Pico, no ip65 change):**

1. **Promiscuous / raw input hook in the CYW43/lwIP stack**
   - If the Pico SDK or lwIP has a hook for “every received Ethernet frame” (e.g. before or after IP processing), call `U2_Net_FeedMacrawRx(0, frame, len)` there when Socket 0 is in MACRAW. This is the cleanest if available.

2. **Duplicate delivery: IP path + MACRAW path**
   - When lwIP receives a frame, first push the **raw** frame (or a copy) to the W5100 emulation via `U2_Net_FeedMacrawRx(0, ...)` if socket 0 is MACRAW; then let lwIP continue as usual. That requires access to the raw buffer and length at the netif input layer (e.g. in the CYW43 poll path or in a custom netif input). Not all ports expose that easily.

3. **Separate “raw” or “packet” socket**
   - Some lwIP ports support a raw PCB or a “packet” interface that receives L2 frames. If present, a small shim could forward those to `U2_Net_FeedMacrawRx(0, ...)`.

4. **Custom netif or shim**
   - Add a layer that receives every frame from the CYW43 driver and (a) pushes it to the W5100 MACRAW RX path when socket 0 is MACRAW, and (b) optionally still feeds lwIP for normal IP stack behaviour. This may require modifying how the Pico W’s netif is registered or how the driver calls `netif->input()`.

**Recommendation:** First determine where in the Pico SDK / CYW43 / lwIP pipeline the **raw Ethernet frame** (and length) is available. Then add a single call to `U2_Net_FeedMacrawRx(0, data, len)` there when U2 socket 0 is open in MACRAW, with no changes to ip65.

### 3.4 Command register “busy” semantics (optional)

Real W5100 keeps Socket Command Register non-zero until the command completes. MegaFlash currently executes OPEN/SEND/RECV etc. synchronously and does not emulate a “busy” period. ip65 waits for Command = 0 before proceeding; if we never set Command to 0, it would spin. So we must either:

- Clear the command register to 0 after handling each command (so the 6502 sees “done” immediately), or  
- Emulate async completion and clear it later.

Current code stores the written value in `u2_memory[address]` for SN_CR; the **read** path returns `u2_memory[address]`. So after we write 0x01 (OPEN) or 0x40 (RECV), that value stays there until overwritten. ip65 **waits** for `data` (Command) to be 0 before sending or before considering receive done. So we **must** clear the stored Command to 0 after handling each command (OPEN, SEND, RECV, etc.). Checking the code: we do `u2_memory[address] = value` in `write_socket_register` and then handle the command; we don’t clear it. So ip65 would see Command = 0x01 or 0x40 forever and hang. **Adaptation:** After handling each command in `write_socket_register`, set `u2_memory[address] = 0` (and, if needed, the same for the socket’s CR location) so that the next read of the Command register returns 0.

---

## 4. Summary: what to change in MegaFlash (no ip65 changes)

| Item | Where | What to do |
|------|--------|------------|
| **RECV advances RX_RD** | `uthernet2.c` | In SN_CR = RECV handler, set Socket’s RX_RD0/RX_RD1 in `u2_memory` to current `sn_rx_wr` (with socket mask) so RSR goes to 0. |
| **Socket Command = 0 after command** | `uthernet2.c` | After handling OPEN, SEND, RECV, CLOSE, etc. in `write_socket_register`, write 0 to the socket’s Command Register in `u2_memory` so ip65’s “wait for command done” loop sees 0. |
| **MACRAW RX feed** | `uthernet2_net.c` | **Done:** When socket 0 is opened in MACRAW, the netif input is replaced with `u2_netif_input_wrapper`, which copies the received frame into the W5100 RX buffer via `push_rx_macraw_cb(0, buf, len)` then calls the original `netif->input`. Restored when socket 0 is closed. |
| **RMSR default 0x06 (optional)** | `uthernet2.c` `u2_reset()` | Set `u2_memory[W5100_RMSR] = 0x06` and same for TMSR if you want to avoid ip65’s “not 0x06 → full reset” path on first init. |

---

## 5. References

- **ip65:** `drivers/ethernet.s`, `drivers/w5100.s`, `drivers/uthernet2.s`, `drivers/w5100driver.s`.
- **MegaFlash:** `pico/uthernet2.c`, `pico/uthernet2_net.c`, `pico/w5100_regs.h`, `pico/busloop.c` (C0x4–C0x7 dispatch).
- **W5100:** Datasheet register map (MR, RTR, RMSR/TMSR, SHAR, Sn_MR, Sn_CR, Sn_SR, Sn_TX_FSR, Sn_TX_WR/RD, Sn_RX_RSR, Sn_RX_RD, MACRAW mode, RECV/SEND semantics).

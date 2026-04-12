## 2026-04-10
- **`build-both.sh`:** Passes **`U2_ETH_HEADER_TRACE`** (**default** **0**); **`U2_ETH_HEADER_TRACE=1 ./build-both.sh`** builds **Release** with UART **`[u2eth]`** trace. Verified **`pico2_release`** **`CXX_DEFINES`** includes **`U2_ETH_HEADER_TRACE=1`**.

- **`U2_ETH_HEADER_TRACE`:** Optional UART **`[u2eth]`** hex (**first** **64** **B** per **STA** **TX**/**RX** **frame**) via **`netif`** **wrap** **in** **`uthernet2_net.cpp`**; **CMake** **`U2_ETH_HEADER_TRACE`** (**default** **0**); **`U2_ETH_HEADER_TRACE=1 ./build-debug-both.sh`**. **§1j** **tracing** **paragraph** **`docs/Implementation-notes-and-reasoning.md`**.

- **CYW43 STA netif + TCP bind (§1j):** **`U2_Net_SendMacraw`**, **MACRAW** **`input`** hook, and **SHAR** on **OPEN** use **`cyw43_state.netif[CYW43_ITF_STA]`** instead of **`netif_default`** / **`netif_list`** — **`netif_set_default`** can leave default on **AP** (**`w1`**), so **`linkoutput`** was sending **MACRAW** on the wrong interface; **`netif_list`** head may not be STA. **`U2_Net_ConnectTcpEx`** now **`tcp_bind`**(**Sn_PORT**) before **`tcp_connect`**. **`./build-both.sh`** ok. **`docs/Implementation-notes-and-reasoning.md`** §1j.

- **DHCP BOOTP `chaddr` + UDP checksum (§1j):** **`U2_Net_SendMacraw`** detects IPv4 **UDP 68→67** **BOOTREQUEST**, patches **chaddr** (payload +28) to **`netif->hwaddr`**, recomputes **UDP** checksum with **`inet_chksum_pseudo`**; reapplies **`U2_SetStationMacFromBytes`** per send. Fixes wget65 DHCP timeout when the server unicasts using **chaddr** while Ethernet **SA** alone was already patched. **`./build-both.sh`** ok → **`pico2_release/megaflash.uf2`**. **`docs/Implementation-notes-and-reasoning.md`** §1j.

- **ip65 DHCP over MACRAW (§1j):** On **`U2_Net_OpenMacraw`**, **`U2_SetStationMacFromBytes(netif_default->hwaddr)`**; **`U2_Net_SendMacraw`** overwrites Ethernet **SA** (bytes 6–11) with **CYW43 STA** MAC so AP accepts frames / DHCP replies route correctly. **`uthernet2.h`** `extern "C"` for C++ link. **`docs/Implementation-notes-and-reasoning.md`**.

- **U2 pipeline streamlining (RP2350):** **`__time_critical_func`** on **`U2_HandleBusAccess`**, **`U2_PeekDataPort`**, **`read_value`/`read_value_at`**, **`write_value`/`write_value_at`**, **`write_common_register`**, **`auto_increment`**, **`set_rx_sizes`/`set_tx_sizes`** — run from SRAM vs XIP on every **`$C0C4–$C0C7`** cycle. §1d in **`docs/Implementation-notes-and-reasoning.md`**.

- **`busloop.c`:** Removed **C0C4 diagnostic LED** (1 s **`ActLed`**) and **`pico/time.h`**; §9 historical note in **`docs/Implementation-notes-and-reasoning.md`**.

- **§1i clarification:** Same **`a2bus`** SM drives **chunk 0 and chunk 1** — no separate “U2 Φ2” vs storage; U2 differences are **CPU/pipeline** (IRQ0, **`U2_PeekDataPort`**, handler work). Scope/Φ2 for **whole-card** analog validation if needed. **`docs/Implementation-notes-and-reasoning.md`**.

- **AppleWin reference (docs):** **`docs/Implementation-notes-and-reasoning.md`** §**1i** — **`Uthernet2::IO_C0`** synchronous I/O vs Pico FIFO/PIO; **`MemReadFloatingBus`**; **`addr & 0x03`** mirrors vs MegaFlash **`$C0C4–$C0C7`** only + PIO chunk = **A3:A2**. **`docs/Uthernet-II-emulation-on-MegaFlash.md`** overview bullet updated; summary table row.

- **`./build-both.sh`:** Success — **`pico_release/megaflash.uf2`**, **`pico2_release/megaflash.uf2`** (Arm GNU Toolchain **`/Applications/ArmGNUToolchain/15.2.rel1/...`**; regenerates **`a2bus_rp2350.pio.h`**).

- **`pico/a2bus_rp2350.pio` + `pico/a2bus.h`:** Shortened **read_cycle** side-delays before **`mov osr,rxfifo[y]`** so the PIO drives the data bus earlier (hypothesis: 6502 sampled too soon vs late FIFO/`out pins`). **`__dsb`** after **`__dmb()`** on **`rxf_putget`** write. **`docs/Implementation-notes-and-reasoning.md`** §1d (PIO timing paragraph).

- **`tools/wget65-verbose/wget65.c`:** Slot **4** fixed; **`videomode(VIDEOMODE_80COL)`**; on **`ip65_init`** failure print **`ip65_error`** + note post-**`eth_init`** dump vs **`w5100.s`** probe (§1h); rebuilt **`wget65v.bin`** / **`FLASHVALID.po`**.

- **`WGET65V` disk + build:** **`WGET65V_BIN`** default path **`../wget65-verbose/wget65v.bin`**; shallow **`ip65/`** at repo root (gitignored) for **`make`** in **`tools/wget65-verbose`**. Built **`wget65v.bin`**, regenerated **`FLASHVALID.po`** with **WGET65V** + **WGET65V.SYSTEM** + launcher + doc. **`docs/Implementation-notes-and-reasoning.md`** §1h.
- **`pico/a2bus.h` (RP2350 only):** `__dmb()` after `rxf_putget[SM_A2BUS][chunk]` write in `UpdateMegaFlashRegisters()` so PIO SM1 sees updated chunk data before the next 6502 read — targets ip65 RTR XOR failure on-bus while UART still logs correct emulated DATA reads (only two `[u2] DATA read` lines). Documented in **`docs/Implementation-notes-and-reasoning.md`** §1d.
- **Hardware scope:** Uthernet II / ip65 bring-up is validated only on **Pico 2 W (RP2350, `pico2_w`)** — use **`pico2_release/megaflash.uf2`** (or **`pico2_debug/`** for UART trace); RP2040 / Pico W is out of scope for current testing.

## 2026-04-03
- **`tools/flash-validate/FLASHSOAK/`:** Finished C port API alignment (`mf_format_disk`, `checksum_volume` + `rd_err`, `mf_read_block` MS/RE), fixed REFORMAT logging and `FILL.TXT` open failure path, cc65-safe `status_line21`; added **`Makefile`** → **`flashsoak.bin`** (`cl65 -t apple2enh`, `cpanel/apple2enh-bin.cfg`). Documented in **`docs/Implementation-notes-and-reasoning.md`** §19. Verified **`make`** builds cleanly.

## 2026-04-05
- Added `tools/flash-validate/FLASHVAL.BAS` (Applesoft) + `README.md`: non-destructive MegaFlash command suite, `FLASHVAL1` baseline file, compare mode; noted in `docs/Implementation-notes-and-reasoning.md` §17.
- `flash.c`: `ChipIDToCapacity()` matches JEDEC memory type + capacity only (drops manufacturer byte) so non-Winbond drop-ins with same codes boot; documented in `docs/Implementation-notes-and-reasoning.md` §16.
- Documented flash/vendor PDF location as `../datasheets/` (repo root) in `README.md` so it matches the user’s layout.

## 2026-04-09
- **Policy:** Uthernet II work stays in **`pico/`** only — **no** patches to upstream **`ip65`** (stock stack/drivers remain the reference). Removed the brief **`ip65/drivers/ethernet*.s`**-related note from **`docs/Implementation-notes-and-reasoning.md`** §1c; **`ip65`** tree matches unmodified upstream.
- **`pico/busloop.c` (RP2350 U2 branch):** Always wait for PIO IRQ 0 to clear before `UpdateMegaFlashRegisters(1,…)` (removed `pio_sm_is_rx_fifo_empty` guard on this path only) so `$C0C4–$C0C7` chunk-1 data is not stale during the ip65 W5100 RTR probe — addresses wget65/telnet65 “device not found” when slot 4 is already correct.
- **`pico/uthernet2.c`:** `write_common_register()` now persists writes to RTR/RCR/PTIMER and other `$0001–$002F` registers not covered by the explicit GAR/SHAR/RMSR branches (previously dropped).
- Added **`pico/build-debug-both.sh`**: `CMAKE_BUILD_TYPE=Debug` + U2 diagnostics (`UTHERNET2_DEBUG`, `U2_ACTIVITY_MONITOR`); default `U2_IP65_TRACE_DATA=1`, `U2_IP65_CHECKPOINT=0` (override via env). Passes `-DPIOASM_INSTALL_DIR=pico_release/pioasm-install` when present so host **pioasm** matches `./build-both.sh` (avoids wrong-architecture pioasm in fresh debug dirs). Outputs **`pico_debug/megaflash.uf2`** and **`pico2_debug/megaflash.uf2`**. **`CMakeLists.txt`** comment points to the script.

## 2026-04-08
- Updated `tools/flash-validate/TFTPUTIL.BAS`: force `PR#3` 80-column startup, remove manual slot prompt, probe slot 4 first then scan 1-7 for MegaFlash (`CMD_GETDEVSTATUS` heuristic), and clarify host prompt as FQDN/IP.
- Added `TFTPUTIL.CFG` read/write behavior in `TFTPUTIL.BAS` so the chosen TFTP host is persisted and offered as next-run default; documented rationale/outcome in `docs/Implementation-notes-and-reasoning.md` §20.
- Added `tools/flash-validate/TFTPUTIL.TXT` and updated `tools/flash-validate/build-flashval-disk.sh` to place it on `FLASHVALID.po` as `TFTPUTIL.DOC` (with README/docs note) so TFTP usage guidance is available directly on disk.
- Enhanced `tools/flash-validate/TFTPUTIL.BAS` unit selection UX: list available volumes as `unit - name` via `CMD_GETDEVSTATUS`/`CMD_GETVOLINFO` before prompting, and enforce input range `1..UC`; updated docs (`README`, `TFTPUTIL.TXT`, implementation notes §22).
- Removed `TFTPUTIL.BAS` menu option `3` (local `CATALOG` listing); TFTP has no server directory list, and the feature was not useful as implemented. Reverted docs (`TFTPUTIL.TXT`, `README`, removed implementation notes §23).

## 2026-03-17
- Expanded `docs/SPI-bus-SD-card-and-flash.md` with concrete SPI trace-length guidelines at 75 MHz vs 25 MHz for flash and SD-card operation, including when to reduce clock or add series damping.

- **Uthernet II net on NetworkPump:** `uthernet2_net.cpp` (was `.c`): pump `AddSession`, `CreateUdpPcb`, `PollOnce`; `INetworkSession::OnUdpRecvPbuf(udp_pcb*,…)`; `cmake` `uthernet2_net.cpp`. See repo `SESSION_LOG.md` + `docs/Implementation-notes-and-reasoning.md` §14.10b.

- Investigated current TFTP-start stall risk after always-on Wi-Fi changes: `udptask.cpp`, `network.cpp`, `network_pump.cpp`, `main.c`, `cmdhandler.c`. Confirmed the prior always-on init/deinit regression is mitigated (`cyw43_is_initialized` guard + manager flow), with remaining edge risk only if startup `RunNTP()` consumes most/all of the 30s `DoTFTPRun()` wait window.

Append new entries above this line


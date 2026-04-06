## 2026-04-05
- Added `tools/flash-validate/FLASHVAL.BAS` (Applesoft) + `README.md`: non-destructive MegaFlash command suite, `FLASHVAL1` baseline file, compare mode; noted in `docs/Implementation-notes-and-reasoning.md` §17.
- `flash.c`: `ChipIDToCapacity()` matches JEDEC memory type + capacity only (drops manufacturer byte) so non-Winbond drop-ins with same codes boot; documented in `docs/Implementation-notes-and-reasoning.md` §16.
- Documented flash/vendor PDF location as `../datasheets/` (repo root) in `README.md` so it matches the user’s layout.

## 2026-03-17
- Expanded `docs/SPI-bus-SD-card-and-flash.md` with concrete SPI trace-length guidelines at 75 MHz vs 25 MHz for flash and SD-card operation, including when to reduce clock or add series damping.

- **Uthernet II net on NetworkPump:** `uthernet2_net.cpp` (was `.c`): pump `AddSession`, `CreateUdpPcb`, `PollOnce`; `INetworkSession::OnUdpRecvPbuf(udp_pcb*,…)`; `cmake` `uthernet2_net.cpp`. See repo `SESSION_LOG.md` + `docs/Implementation-notes-and-reasoning.md` §14.10b.

Append new entries above this line


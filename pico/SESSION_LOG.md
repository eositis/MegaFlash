## 2026-03-17
- Expanded `docs/SPI-bus-SD-card-and-flash.md` with concrete SPI trace-length guidelines at 75 MHz vs 25 MHz for flash and SD-card operation, including when to reduce clock or add series damping.

- **Uthernet II net on NetworkPump:** `uthernet2_net.cpp` (was `.c`): pump `AddSession`, `CreateUdpPcb`, `PollOnce`; `INetworkSession::OnUdpRecvPbuf(udp_pcb*,…)`; `cmake` `uthernet2_net.cpp`. See repo `SESSION_LOG.md` + `docs/Implementation-notes-and-reasoning.md` §14.10b.

Append new entries above this line


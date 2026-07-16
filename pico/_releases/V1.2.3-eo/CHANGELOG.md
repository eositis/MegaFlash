# V1.2.3-eo

**1.2 series:** Uthernet II emulation on Pico 2 W (RP2350). Use **`megaflash-pico2.uf2`** from this folder.

## Uthernet II / ip65 (§10l + §10k–§10za)

- **RX ring pointer geometry (§10l):** Monotonic `sn_rx_wr`, coherent `Sn_RX_RD` reads, and correct occupancy math — fixes periodic Contiki wget corruption at ~1390-byte boundaries.
- **Ingress split + poll cadence (§10k/§10w/§10x):** While ip65 MACRAW is active, inbound frames go to the emulated W5100 ring only (no duplicate lwIP feed / spurious RST). Core 0 polls network before IPC FIFO wait; `IPCCMD_NET_WAKE` on SEND/RECV.
- **MACRAW TX ring (§10z):** Depth-16 deferred TX queue on RP2350; honest `Sn_TX_RD` advance only when enqueue succeeds.
- **Duplicate TX race fix (§10za):** Removed core-0 pending-SEND retry during `Sn_CR=SEND`; wake core 0 **after** `send_data()` — fixes broken telnet/wget handshakes while browser stayed OK.

## Validation (2026-06-27, §10za firmware)

- **telnet65:** DHCP and DNS work (brief ARP retry at session start); TCP sessions establish to local telnetd and WAN hosts.
- **Contiki:** Web browsing OK.
- **Contiki wget:** Still unreliable on large downloads (bulk RX / checksum — open issue).

## Notes

- Uncommitted experiments (RX/TX reset on OPEN, MACRAW RX staging) were **not** included — they regressed DHCP/DNS and were reverted before this release.

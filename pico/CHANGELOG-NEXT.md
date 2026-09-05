# @VERSION@

**1.2 series:** Uthernet II emulation on Pico 2 W (RP2350). Use **`megaflash-pico2.uf2`** from this folder.

## Uthernet II / ip65 (first-connect + checksums)

- **MACRAW wrap checksums (§1dq / §1dx):** On a straddling RX record, lay bytes where the ip65/Contiki driver actually reads them (including two bytes past the ring when the neighbour socket is CLOSED). Fixes IP/TCP checksum errors every ~3rd/6th HTTP segment.
- **First TCP connect (§1ec):** Remap Contiki/ip65 source port 1026 to a **new ephemeral** (49152–57342) on each SYN, with an incremental TCP checksum. A fixed wire port (41226) reused the same 4-tuple after reboot (TIME-WAIT / conntrack) so the first SYN got no SYN-ACK; 1027/1028 still worked as new 4-tuples. Pad MACRAW TX to 60 bytes (W5100-like).
- **Release hygiene:** FIRSTCONN UART / `U2_FIRSTCONN` removed after confirmation. `U2_RX_AUDIT` remains a CMake opt-in (default off).

## Validation (2026-09-05)

- First Contiki/wget connect after a fresh reboot succeeds (ephemeral NAT). Checksum wrap shim previously confirmed.

## Notes

- Flash **`megaflash-pico2.uf2`** from this folder (Pico 2 W). Measurement copies under `optionB/` are no longer used.

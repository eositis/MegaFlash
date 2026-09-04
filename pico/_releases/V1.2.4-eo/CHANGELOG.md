# V1.2.4-eo

**1.2 series:** Uthernet II emulation on Pico 2 W (RP2350). Use **`megaflash-pico2.uf2`** from this folder.

## Uthernet II / ip65 (first-connect + checksums)

- **MACRAW wrap checksums (§1dq / §1dx):** On a straddling RX record, lay bytes where the ip65/Contiki driver actually reads them (including two bytes past the ring when the neighbour socket is CLOSED). Fixes IP/TCP checksum errors every ~3rd/6th HTTP segment.
- **First TCP connect (§1dz / §1ea):** Remap Contiki ephemeral source port 1026 → 41226 with an incremental TCP checksum; pad MACRAW TX frames to 60 bytes (W5100-like). First Contiki/wget connect no longer waits for a retry port.
- **Release hygiene:** `U2_FIRSTCONN` / `U2_RX_AUDIT` default off — no UART NDJSON instrumentation in this binary.

## Validation (2026-09-04)

- Contiki: first browse connects without retry.
- Contiki/ip65 wget: connects; checksum errors no longer observed (soak testing continues).

## Notes

- Debug / FIRSTCONN measurement images under `optionB/` are not this release.

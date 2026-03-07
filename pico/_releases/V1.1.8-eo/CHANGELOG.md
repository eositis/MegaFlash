# Changelog — V1.1.8-eo (05-Mar-2026)

## C0xx address decode

- **Concurrent C0xx ranges:** C0C0–C0C3 (MegaFlash), C0C4–C0C7 (Uthernet II), and C0C8–C0CF (reserved for future ACIA) are **concurrently active**. Decode is by address only; no GPIO slot select.
- **Uthernet II limited to C0C4–C0C7:** U2 handling uses `addr >= U2_C0X_OFFSET && addr <= U2_C0X_LAST` so C0C8–C0CF remain free for ACIA emulation.

## Uthernet II

- **Read-back fix:** Reads from $C0C4–$C0C7 (e.g. Mode Register after write) now return the correct value by updating PIO chunk 1 (`UpdateMegaFlashRegisters(1, ...)`) instead of chunk 0.
- **C0C4 diagnostic LED:** Any access to $C0C4 turns on the activity LED for 1 second (non-blocking).

## Build and release

- Version bump and build date applied on each `cmakeall.sh` run; release UF2s copied to `_releases/<version>/` as `megaflash-pico.uf2` and `megaflash-pico2.uf2`.

## Documentation

- **Implementation notes (§1b):** Documents concurrent C0xx design and U2 restricted to 4–7.
- **busloop.c:** Comment updated to describe C0x0–C0x3 (MegaFlash), C0x4–C0x7 (U2), C0x8–C0xF (reserved for ACIA).

## Hardware / GPIO (prior session)

- A2/A3 and nDEVSEL pull configuration per Implementation notes; nDEVSEL pull-up re-enabled so the PIO sees a clean low when the card is selected.

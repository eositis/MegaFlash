# V1.1.13-eo

## UDP/TFTP heartbeat reverted to 50 ms

- **HEARTBEAT_PERIOD:** Reverted from 10 ms back to 50 ms in `udptask.h`. The shorter period caused TFTP upload timeouts and crashes (lwIP re-entrancy risk).
- **TFTP RX buffering** (V1.1.11) remains: incoming DATA packets are queued during flash erase, so the 50 ms loop is sufficient for most transfers.

## Control Panel: ROM Disk row in Drives Enable

- **ROM Disk visibility:** The ROM Disk checkbox row was not shown because it overwrote the last drive row. Fixed: ROM Disk row is now at `YPOS+unitCount+1`; drive checkboxes use `YPOS+i`. ROM Disk appears on the line after the last drive.
- **cpanel:** Custom `Beep()` in `asm-megaflash.s` (cc65 2.18 lacks `beep`). Drives Enable C89 fixes. `cpanel.bin` rebuilt and embedded in firmware.

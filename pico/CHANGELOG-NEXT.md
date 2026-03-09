# @VERSION@

## WiFi kept online between sessions

- **No teardown on session end:** The CUDPTask destructor no longer disconnects or deinitializes WiFi when a TFTP/NTP/TestWifi session completes. Only the UDP PCB and RX buffer are freed; CYW43 and the connection stay up.
- **Fast restart:** Subsequent TFTP/NTP sessions skip InitCyw43 (static guard) and ConnectWifi returns immediately when already LINK_UP. TFTP starts much faster when run again soon after a previous session.
- **Power:** WiFi stays on at all times; no power saving. `pico/udptask.cpp`.

## Control Panel changes

- **Drives Enable: ROM Disk row fixed:** The ROM Disk checkbox row was missing because it overwrote the last drive (RAM Disk). Fixed: ROM Disk at `YPOS+unitCount+2`; checkbox rows aligned with `PrintDriveList` using `YPOS+1+i`. `cpanel/drivesenable.c`.
- **Build ID left of clock:** CMD_GETFIRMWAREVER (0x29) polls firmware version separately (ProDOS-safe). DisplayTime shows build ID at cols 20–31, time at 32–39. `common/defines.h`, `pico/cmdhandler.c`, `cpanel/asm-megaflash.s`.

## TFTP reverted to original state

- **Reverted:** All TFTP-related changes (RX buffering, session cleanup, UDP timing) reverted for stability. Restored: `udptask.cpp`, `udptask.h`, `tftprxtask.cpp`, `tftprxtask.h`, `flash.c`, `flash.h`, `network.cpp`, `tftpstate.c`, `tftpstate.h` to pre-modification state.

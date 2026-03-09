# V1.1.15-eo

## WiFi kept online between sessions

- **No teardown on session end:** The CUDPTask destructor no longer disconnects or deinitializes WiFi when a TFTP/NTP/TestWifi session completes. Only the UDP PCB and RX buffer are freed; CYW43 and the connection stay up.
- **Fast restart:** Subsequent TFTP/NTP sessions skip InitCyw43 (static guard) and ConnectWifi returns immediately when already LINK_UP. TFTP starts much faster when run again soon after a previous session.
- **TFTP receive fix (reuse path):** When reusing WiFi, ConnectWifi now runs a ~100 ms warmup (100× `cyw43_arch_poll()` + 1 ms) before returning. This stabilizes lwIP/CYW43 so the new UDP PCB can receive (addresses pico-sdk #915 “sending but not receiving”). `udptask.cpp`.
- **Power:** WiFi stays on at all times; no power saving. `pico/udptask.cpp`.

## Control Panel changes

- **Drives Enable: ROM Disk row fixed:** The ROM Disk checkbox row was missing because it overwrote the last drive (RAM Disk). Fixed: ROM Disk at `YPOS+unitCount+2`; checkbox rows aligned with `PrintDriveList` using `YPOS+1+i`. `cpanel/drivesenable.c`.
- **Drives Enable: ROM disk shown once, checkboxes aligned:** ROM disk was listed twice (in the drive list and on the custom row). Added `GetDriveListCount()` to exclude ROM disk from the list when it is the last unit; ROM disk now appears only on its own row. Checkbox rows corrected to `YPOS+i` so they align with drive lines. `cpanel/ui-misc.c`, `drivesenable.c`.
- **ROM disk excluded from TFTP and Format:** ROM disk is read-only and not formatable; it no longer appears in the TFTP drive-selection list or Format drive-selection list. `cpanel/tftp.c`, `cpanel/format.c`.
- **Build ID left of clock:** CMD_GETFIRMWAREVER (0x29) polls firmware version separately (ProDOS-safe). DisplayTime shows build ID at cols 20–31, time at 32–39. `common/defines.h`, `pico/cmdhandler.c`, `cpanel/asm-megaflash.s`.

## TFTP reverted to original state

- **Reverted:** All TFTP-related changes (RX buffering, session cleanup, UDP timing) reverted for stability. Restored: `udptask.cpp`, `udptask.h`, `tftprxtask.cpp`, `tftprxtask.h`, `flash.c`, `flash.h`, `network.cpp`, `tftpstate.c`, `tftpstate.h` to pre-modification state.

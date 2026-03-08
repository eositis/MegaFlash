# Changelog — V1.1.9-eo (08-Mar-2026)

## Control Panel — Drives Enable

- **ROM Disk show/hide:** Drives Enable page now has a **ROM Disk** line. Use key **R** to toggle; **Enter** applies and sends the choice to the Pico (show at last unit or hide). Config bit **ROMDISKFLAG** in configbyte1 (1 = show, 0 = hide); default for new configs is show.
- **Layout:** One blank line removed before the prompt so the page stays within the 24-line screen.

## Pico firmware — ROM disk config

- **ReconfigRomdisk():** Cold start and config load apply ROMDISKFLAG: if set, ROM disk is enabled at last unit; if clear, ROM disk is hidden. DoAppleColdStart() no longer always enables ROM disk; config controls it.
- **DisableRomdisk:** New command CMD_DISABLEROMDISK; Control Panel asm `DisableRomdisk()` for the “hide” option.

## Hardware / GPIO

- **nDEVSEL pull disabled:** GPIO 20 (nDEVSEL) has no internal pull; `gpio_set_pulls(nDEVSEL_GPIO, false, false)` in both PIO inits (RP2040 and RP2350). Line is driven by the Apple bus.

## Build and release

- Version bump and build date on each `cmakeall.sh` run; UF2s in `_releases/<version>/` as `megaflash-pico.uf2` and `megaflash-pico2.uf2`.

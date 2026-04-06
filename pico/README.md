# Pico Firmware

## Build environment

For full requirements (SDK, ARM toolchain, CMake, Control Panel prerequisite, and how to recreate the build on another machine), see **[BUILD-REQUIREMENTS.md](BUILD-REQUIREMENTS.md)**.

Summary: clone [pico-sdk](https://github.com/raspberrypi/pico-sdk) to **`~/pico-sdk`** (or set **`PICO_SDK_PATH`**). On **Apple Silicon**, install **arm64** CMake (e.g. Homebrew **`/opt/homebrew`**); **`build-env.sh`** picks that **`cmake`** before **`/usr/local`**. From the MegaFlash `pico` directory run:

```
./cmakeall.sh
```

To **build both boards** (Pico W + Pico 2 W Release) **without** bumping the firmware version in `defines.h` (e.g. CI or a quick compile check):

```
./build-both.sh
```

To build **Debug** UF2s for both boards (UART 115200, `[u2]` W5100 trace + **`[u2m]`** U2 activity monitor: every `$C0C4–$C0C7` bus cycle, socket commands, and lwIP RX/TX). Run **`./cmakeall.sh` once** so `pico_debug` / `pico2_debug` exist, then:

```
./build-debug.sh
```

Capture serial and filter on `grep u2m` (or `[u2m]`) to follow the queue-drained monitor lines. Heavy traffic can fill the 128-entry ring; the firmware prints a **dropped** warning if events were lost.

Each run passes **`FIRMWARE_BUILD_TIMESTAMP`** (Unix seconds) and **`FIRMWARE_BUILD_TIMESTAMP_STR`** (UTC, e.g. `2026-03-21 12:34:56 UTC`) into CMake; **`build_id.h`** is generated with both. The UF2 embeds Unix time in device info / `CMD_GETFIRMWAREVER` bytes 12–15 and shows the human-readable string in the USB device-info text (and Debug UART). Override either variable before running if needed.
If everything is correct, the following sub-directories should be created.

```
pico_debug
pico_release
pico2_debug
pico2_release
picotool
```

Note: You need to execute the shell script only once unless `CMakeLists.txt` file is changed or you want to recreate the build directories.

`pico_debug` and `pico_release` are the build directories for Pico Board (RP2040).  `pico2_debug` and `pico2_release` are the build directories for Pico2 Board (RP2350).

## Datasheets (flash and related parts)

Vendor PDFs (e.g. Winbond, Alliance) for SPI flash and related ICs live in the **MegaFlash repo root**, next to `pico/`:

- From this directory: **`../datasheets/`** (i.e. `MegaFlash/datasheets/`).

That folder is for human reference when comparing command sets, SFDP, and status-register layouts with what `flash.c` assumes.

## Build Instruction

Before compiling the pico firmware, the control panel must be built first. Please follow the instruction in `cpanel` directory to build the control panel binary.

To build the pico firmware, go to one of the build directory e.g. `pico2_release`. Then, execute `make`. The output file is `megaflash.uf2`.




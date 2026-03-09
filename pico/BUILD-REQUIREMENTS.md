# Pico firmware — build requirements

This document describes what is needed to build the MegaFlash Pico firmware on a new machine so that the same UF2 images can be recreated.

## Supported hosts

- **Linux** (primary; original target)
- **macOS** (tested; ARM toolchain and `sed` behaviour may differ — see below)

## 1. Raspberry Pi Pico SDK

The firmware uses the official [Raspberry Pi Pico C/C++ SDK](https://github.com/raspberrypi/pico-sdk).

- **Obtain:** Clone the repo, then initialize **all required submodules** (see list below):
  ```bash
  git clone https://github.com/raspberrypi/pico-sdk
  cd pico-sdk
  git submodule update --init
  ```
  If you use a release tag, check out the tag before running `git submodule update --init` so submodule commits match that release.
- **Version:** Use a recent stable release or the tag that matches your SDK clone. The project has been built with SDK versions that support both **Pico W (RP2040)** and **Pico 2 W (RP2350)**.
- **Path:** Set the environment variable **`PICO_SDK_PATH`** to the absolute path of the SDK directory (e.g. `export PICO_SDK_PATH=/path/to/pico-sdk`).  
  The build script `cmakeall.sh` uses this; if unset, it will use a default path that you must change for your machine (see script comments).

### 1.1 Pico SDK add-ons (externals / submodules) required for this build

The MegaFlash firmware is built for **Pico W** and **Pico 2 W**, which use Wi-Fi (and optionally Bluetooth). The following SDK externals are **required**; they are Git submodules of `pico-sdk` and must be present after `git submodule update --init`:

| Submodule        | Path in SDK    | Purpose |
|------------------|----------------|--------|
| **tinyusb**      | `lib/tinyusb`  | USB stack (stdio over USB, device support) |
| **cyw43-driver** | `lib/cyw43-driver` | Wi-Fi/Bluetooth driver for Pico W and Pico 2 W |
| **lwip**         | `lib/lwip`     | TCP/IP stack (UDP, TCP, DHCP, DNS; used with CYW43 for networking) |
| **mbedtls**      | `lib/mbedtls`  | TLS/crypto (used by CYW43 and Wi-Fi auth) |
| **btstack**      | `lib/btstack`  | Bluetooth stack (Pico W / Pico 2 W Bluetooth support) |

The project links `pico_cyw43_arch_lwip_poll` (see `CMakeLists.txt`), which pulls in **cyw43-driver** and **lwip**; **tinyusb** is used for USB stdio; **mbedtls** and **btstack** are brought in by the SDK when building for `pico_w` / `pico2_w`. If any of these are missing, CMake configuration or the build will fail. Ensure all five submodules are initialized (no need to clone them separately — `git submodule update --init` from the SDK root fetches them).

## 2. ARM GCC toolchain (bare-metal)

The RP2040 and RP2350 are built with the **arm-none-eabi** toolchain (no OS, bare-metal).

- **Required programs:** `arm-none-eabi-gcc`, `arm-none-eabi-g++`, `arm-none-eabi-ar`, `arm-none-eabi-ranlib` (and `arm-none-eabi-as` via gcc).
- **Obtain:**
  - **macOS:** [Arm GNU Toolchain for Arm Embedded Processors](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (e.g. “Arm GNU Toolchain arm-none-eabi”). Install so that the `arm-none-eabi/bin` directory is available; the script looks under `/Applications/ArmGNUToolchain/*/arm-none-eabi/bin` or uses `ARM_TOOLCHAIN_PATH` (see below).
  - **Linux:** Install the package (e.g. `sudo apt install gcc-arm-none-eabi` on Debian/Ubuntu) or use the same Arm GNU Toolchain tarball.
- **Important:** The build script explicitly passes the C/C++/AR/RANLIB paths to CMake so that the correct toolchain is used (e.g. on macOS, Xcode’s `ranlib` must not be used). Use a toolchain that provides `nosys.specs` and is intended for bare-metal.
- **Optional override:** Set **`ARM_TOOLCHAIN_PATH`** to the **bin** directory containing `arm-none-eabi-gcc` (e.g. `export ARM_TOOLCHAIN_PATH="/Applications/ArmGNUToolchain/12.3.rel1/arm-none-eabi/bin"`) to force a specific toolchain.

## 3. CMake and Make

- **CMake:** Version **3.13 or later** (up to 3.27). The top-level `CMakeLists.txt` requires `cmake_minimum_required(VERSION 3.13...3.27)`.
- **Make:** Standard `make` for the configured build directories.

Install via your system package manager or from [cmake.org](https://cmake.org/download/).

## 4. Control panel binary (prerequisite)

The Pico firmware links in the **Control Panel** binary. It must be built **before** building the Pico firmware.

- **Location:** Project `cpanel/` (sibling of `pico/`).
- **Requirements:** CC65 toolchain; see `cpanel/README.md` (e.g. `make release`).
- **Output:** `cpanel/cpanel.bin` must exist. The Pico `CMakeLists.txt` lists `cpanel.s` with `OBJECT_DEPENDS` on `../cpanel/cpanel.bin`, so a missing or stale `cpanel.bin` can cause incorrect or failed builds.

## 5. Romdisk image (optional / project-specific)

The firmware can include a ROM disk image via `romdisk.s`, which includes the binary file **`romdisk.po`** from the `pico/` directory. If your tree or build process provides `romdisk.po`, it will be embedded; if not, you may need to generate or obtain it per project instructions (or use an empty placeholder if the feature is unused).

## 6. Build script and version bump

- **Script:** Run **`./cmakeall.sh`** from the **`pico/`** directory.
- **Behaviour:** The script (1) bumps the firmware version and date in `defines.h`, (2) configures four CMake build trees, (3) builds the two release trees, and (4) copies the UF2 files into **`_releases/<version>/`** as `megaflash-pico.uf2` and `megaflash-pico2.uf2`. A **`CHANGELOG.md`** is always included: if **`CHANGELOG-NEXT.md`** exists, it is copied with `@VERSION@` replaced by the build version; otherwise a stub is created.
- **`sed` and macOS:** The script uses `sed -i.bak` for in-place edits. This is compatible with **BSD sed** (macOS). On **GNU sed** (Linux), `sed -i.bak` also works. If you port to a system where in-place `sed` differs, adjust the `sed` invocations in `cmakeall.sh`.

## 7. Summary: minimal steps to reproduce the build

1. Install **Raspberry Pi Pico SDK**: clone the repo and run **`git submodule update --init`** in the SDK directory so all add-ons (tinyusb, cyw43-driver, lwip, mbedtls, btstack) are present. Set **`PICO_SDK_PATH`** to the SDK path.
2. Install **arm-none-eabi** GCC toolchain; optionally set **`ARM_TOOLCHAIN_PATH`** to its `bin` directory.
3. Install **CMake** (3.13+) and **Make**.
4. Build the Control Panel: from repo root, `cd cpanel && make release` (ensure **CC65** is installed); then return to `pico/`.
5. Ensure **`romdisk.po`** is present in `pico/` if your build expects it.
6. From the **`pico/`** directory, run **`./cmakeall.sh`**.
7. Collect UF2 images from **`pico/_releases/<version>/`**.

## 8. Boards and outputs

| Build directory   | Board    | MCU   | UF2 in release folder      |
|-------------------|----------|-------|----------------------------|
| `pico_release`    | Pico W   | RP2040| `megaflash-pico.uf2`       |
| `pico2_release`   | Pico 2 W | RP2350| `megaflash-pico2.uf2`      |

Debug builds (`pico_debug`, `pico2_debug`) are configured but not copied to `_releases/` by the script.

# Pico firmware — build requirements

This document describes what is needed to build the MegaFlash Pico firmware on a new machine so that the same UF2 images can be recreated.

## Supported hosts

- **Linux** — x86_64 or **aarch64** (ARM64). Install `gcc-arm-none-eabi` / CMake from your distro (or Arm’s tarball for **aarch64** if you need a specific GCC version).
- **macOS** — **Apple Silicon (ARM64)** and Intel. On Apple Silicon, **Homebrew** lives under **`/opt/homebrew`**. Install **`brew install cmake`** and **`brew install --cask gcc-arm-embedded`** (not the formula **`arm-none-eabi-gcc`**). `sed` behaviour matches BSD sed (see §6).

## 1. Raspberry Pi Pico SDK

The firmware uses the official [Raspberry Pi Pico C/C++ SDK](https://github.com/raspberrypi/pico-sdk). The SDK is **one source tree** for all host CPUs (Intel Mac, Apple Silicon, Linux x86_64/aarch64); there is no separate “ARM SDK” package—clone it on your ARM machine the same way and point **`PICO_SDK_PATH`** at it (or use **`~/pico-sdk`** as below).

- **Obtain:** Clone the repo, then initialize **all required submodules** (see list below):
  ```bash
  git clone https://github.com/raspberrypi/pico-sdk ~/pico-sdk
  cd ~/pico-sdk
  git submodule update --init
  ```
  If you use a release tag, check out the tag before running `git submodule update --init` so submodule commits match that release.
- **Version:** Use a recent stable release or the tag that matches your SDK clone. The project has been built with SDK versions that support both **Pico W (RP2040)** and **Pico 2 W (RP2350)**.
- **Path:** Set the environment variable **`PICO_SDK_PATH`** to the absolute path of the SDK directory (e.g. `export PICO_SDK_PATH=/path/to/pico-sdk`).  
  If unset, **`cmakeall.sh`** and **`build-both.sh`** default to **`$HOME/pico-sdk`**, and **`CMakeLists.txt`** uses the same **`$ENV{HOME}/pico-sdk`** fallback so bare `cmake` runs work. On **Apple Silicon**, keeping the clone at **`~/pico-sdk`** matches the defaults and avoids editing the repo.

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
  - **macOS (preferred):** Homebrew-managed full toolchain + CMake:
    ```bash
    brew install cmake
    brew install --cask gcc-arm-embedded   # needs admin once (installs the Arm .pkg)
    ```
    The cask puts **`arm-none-eabi-*`** shims in **`/opt/homebrew/bin`** (Apple Silicon) or **`/usr/local/bin`** (Intel). Those shims have **`nosys.specs`** (newlib). Build scripts prefer that prefix first.
  - **Not enough for Pico:** **`brew install arm-none-eabi-gcc`** (formula) is a cross-GCC **without newlib**. Scripts reject it. Do **not** substitute it for the **cask**.
  - **Fallback:** [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) **darwin-aarch64** / **darwin-x86_64** `.pkg` under **`/Applications/ArmGNUToolchain/*/arm-none-eabi/bin`**, then **`PATH`**, or **`ARM_TOOLCHAIN_PATH`**.
  - **Linux:** Install the package (e.g. `sudo apt install gcc-arm-none-eabi` on Debian/Ubuntu) or use the Arm GNU Toolchain tarball for **aarch64** or **x86_64** matching your host.
- **Important:** The build script explicitly passes the C/C++/AR/RANLIB paths to CMake so that the correct toolchain is used (e.g. on macOS, Xcode’s `ranlib` must not be used). Use a toolchain that provides `nosys.specs` and is intended for bare-metal.
- **Optional override:** Set **`ARM_TOOLCHAIN_PATH`** to the **bin** directory containing `arm-none-eabi-gcc` (normally leave unset so Homebrew **`/opt/homebrew/bin`** is used).

## 3. CMake and Make

- **CMake:** Version **3.13 or later** (up to 3.27). The top-level `CMakeLists.txt` requires `cmake_minimum_required(VERSION 3.13...3.27)`.
- **Make:** Standard `make` for the configured build directories.

Install via your system package manager or from [cmake.org](https://cmake.org/download/) — on **Apple Silicon**, install the **macOS arm64** build (or `brew install cmake` into **`/opt/homebrew`**).

- **Scripts (`cmakeall.sh`, `build-both.sh`, `build-debug.sh`):** They **`source build-env.sh`**, which sets **`CMAKE_BIN`** to the first executable found among **`/opt/homebrew/bin/cmake`** (Apple Silicon Homebrew), **`/usr/local/bin/cmake`** (Intel Mac / some Linux), then **`cmake`** on **`PATH`**. That avoids using an old **x86_64** CMake when an **arm64** one exists. Override with **`export CMAKE=/path/to/cmake`**.
- **Hand-invoked `cmake`:** If you configure build dirs yourself, call the same binary (e.g. **`/opt/homebrew/bin/cmake`**) or put **`/opt/homebrew/bin`** early in **`PATH`** so **`CMakeCache.txt`** records the correct program.

- **macOS Intel → Apple Silicon:** If **`cmake`** (or **cc65**, **java**, etc.) fails with **`bad CPU type in executable`**, the binary is still **x86_64**. Reinstall for **arm64** so tools match the host CPU.

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
2. Install **arm-none-eabi** GCC with newlib: on macOS **`brew install --cask gcc-arm-embedded`** (admin once). Optionally set **`ARM_TOOLCHAIN_PATH`**.
3. Install **CMake** (3.13+) and **Make** (`brew install cmake` on macOS).
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

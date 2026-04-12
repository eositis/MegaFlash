#!/usr/bin/env bash
# Build Pico W (RP2040) and Pico 2 W (RP2350) Release firmware in one step.
# Does NOT bump defines.h (unlike cmakeall.sh). Use for CI / quick verification.
# Sets FIRMWARE_BUILD_TIMESTAMP (Unix s) and FIRMWARE_BUILD_TIMESTAMP_STR (UTC, human-readable) per configure.
#
# Run from repo:  ./build-both.sh
# Optional:       JOBS=8 ./build-both.sh
# Optional:       U2_ETH_HEADER_TRACE=1 ./build-both.sh   # UART [u2eth] first 64B hex per STA TX/RX frame
#
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=build-env.sh
source "$SCRIPT_DIR/build-env.sh"

JOBS="${JOBS:-8}"
U2_ETH_HEADER_TRACE="${U2_ETH_HEADER_TRACE:-0}"

# --- ARM toolchain (same logic as cmakeall.sh) ---
TOOLCHAIN_BIN=""
if [ -n "${ARM_TOOLCHAIN_PATH:-}" ] && mf_try_arm_toolchain_bin "$ARM_TOOLCHAIN_PATH"; then
  TOOLCHAIN_BIN="$ARM_TOOLCHAIN_PATH"
elif [ -d /Applications/ArmGNUToolchain ]; then
  for d in /Applications/ArmGNUToolchain/*/arm-none-eabi/bin; do
    if mf_try_arm_toolchain_bin "$d"; then
      TOOLCHAIN_BIN="$d"
      break
    fi
  done
fi
# Homebrew: Apple Silicon uses /opt/homebrew; Intel macOS often /usr/local. Prefer before generic PATH.
if [ -z "$TOOLCHAIN_BIN" ]; then
  for _hb in /opt/homebrew /usr/local; do
    if mf_try_arm_toolchain_bin "$_hb/bin"; then
      TOOLCHAIN_BIN="$_hb/bin"
      break
    fi
  done
fi
if [ -z "$TOOLCHAIN_BIN" ]; then
  GCC_PATH=$(command -v arm-none-eabi-gcc 2>/dev/null || true)
  if [ -n "$GCC_PATH" ]; then
    _gdir=$(dirname "$GCC_PATH")
    if mf_try_arm_toolchain_bin "$_gdir"; then
      TOOLCHAIN_BIN="$_gdir"
    fi
  fi
fi

CMAKE_ARM_TOOLCHAIN=""
if [ -n "$TOOLCHAIN_BIN" ]; then
  export PATH="$TOOLCHAIN_BIN:$PATH"
  CMAKE_ARM_TOOLCHAIN="-DPICO_TOOLCHAIN_PATH=$TOOLCHAIN_BIN"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_C_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-gcc"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_CXX_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-g++"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_ASM_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-gcc"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_AR=$TOOLCHAIN_BIN/arm-none-eabi-ar"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_RANLIB=$TOOLCHAIN_BIN/arm-none-eabi-ranlib"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_OBJDUMP=$TOOLCHAIN_BIN/arm-none-eabi-objdump"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_OBJCOPY=$TOOLCHAIN_BIN/arm-none-eabi-objcopy"
  echo "Using ARM toolchain: $TOOLCHAIN_BIN"
else
  echo "Error: no usable arm-none-eabi toolchain (need host-native GCC + newlib, e.g. Arm GNU Toolchain)." >&2
  echo "  macOS Apple Silicon: install the **darwin-aarch64** arm-none-eabi package from https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads" >&2
  echo "  (Intel-only /Applications/ArmGNUToolchain and Homebrew arm-none-eabi-gcc are not sufficient for Pico.)" >&2
  echo "  Then set ARM_TOOLCHAIN_PATH to the .../arm-none-eabi/bin directory, or fix your PATH." >&2
  exit 1
fi

CPANEL_DIR="$(cd "$SCRIPT_DIR/../cpanel" 2>/dev/null && pwd)"
if [ -d "$CPANEL_DIR" ] && [ -f "$CPANEL_DIR/Makefile" ]; then
  echo "Building cpanel (release only; skips test disk + Java)..."
  make -C "$CPANEL_DIR" release || exit 1
else
  echo "Warning: cpanel directory not found, using existing cpanel.bin"
fi

SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"
if [ ! -d "$SDK_PATH" ]; then
  echo "Error: Pico SDK not found at $SDK_PATH. Set PICO_SDK_PATH or clone: git clone https://github.com/raspberrypi/pico-sdk \"\$HOME/pico-sdk\" && (cd \"\$HOME/pico-sdk\" && git submodule update --init)" >&2
  exit 1
fi

echo "Using CMake: $CMAKE_BIN"
echo "U2_ETH_HEADER_TRACE=$U2_ETH_HEADER_TRACE (1=UART [u2eth] STA frame hex dump)"

# Embed configure-time build id (new values each run → identifiable builds without bumping FIRMWAREVER).
FIRMWARE_BUILD_TIMESTAMP="${FIRMWARE_BUILD_TIMESTAMP:-$(date +%s)}"
FIRMWARE_BUILD_TIMESTAMP_STR="${FIRMWARE_BUILD_TIMESTAMP_STR:-$(date -u +"%Y-%m-%d %H:%M:%S UTC")}"
echo "FIRMWARE_BUILD_TIMESTAMP=$FIRMWARE_BUILD_TIMESTAMP  ($FIRMWARE_BUILD_TIMESTAMP_STR)"
echo "(override either by exporting FIRMWARE_BUILD_TIMESTAMP / FIRMWARE_BUILD_TIMESTAMP_STR before run)"

echo "Configuring pico_release (Pico W)..."
"$CMAKE_BIN" -B pico_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  -DU2_ETH_HEADER_TRACE="$U2_ETH_HEADER_TRACE" \
  $CMAKE_ARM_TOOLCHAIN

echo "Configuring pico2_release (Pico 2 W)..."
"$CMAKE_BIN" -B pico2_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  -DU2_ETH_HEADER_TRACE="$U2_ETH_HEADER_TRACE" \
  $CMAKE_ARM_TOOLCHAIN

echo "Building pico_release (-j$JOBS)..."
make -C pico_release -j"$JOBS" || exit 1

echo "Building pico2_release (-j$JOBS)..."
make -C pico2_release -j"$JOBS" || exit 1

echo "OK: pico_release/megaflash.uf2 and pico2_release/megaflash.uf2"

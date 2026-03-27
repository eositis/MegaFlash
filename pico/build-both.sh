#!/usr/bin/env bash
# Build Pico W (RP2040) and Pico 2 W (RP2350) Release firmware in one step.
# Does NOT bump defines.h (unlike cmakeall.sh). Use for CI / quick verification.
# Sets FIRMWARE_BUILD_TIMESTAMP (Unix s) and FIRMWARE_BUILD_TIMESTAMP_STR (UTC, human-readable) per configure.
#
# Run from repo:  ./build-both.sh
# Optional:       JOBS=8 ./build-both.sh
#
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

JOBS="${JOBS:-8}"

# --- ARM toolchain (same logic as cmakeall.sh) ---
TOOLCHAIN_BIN=""
if [ -n "${ARM_TOOLCHAIN_PATH:-}" ] && [ -x "$ARM_TOOLCHAIN_PATH/arm-none-eabi-gcc" ]; then
  TOOLCHAIN_BIN="$ARM_TOOLCHAIN_PATH"
elif [ -d /Applications/ArmGNUToolchain ]; then
  for d in /Applications/ArmGNUToolchain/*/arm-none-eabi/bin; do
    if [ -x "$d/arm-none-eabi-gcc" ]; then
      TOOLCHAIN_BIN="$d"
      break
    fi
  done
fi
if [ -z "$TOOLCHAIN_BIN" ]; then
  GCC_PATH=$(command -v arm-none-eabi-gcc 2>/dev/null)
  [ -n "$GCC_PATH" ] && TOOLCHAIN_BIN=$(dirname "$GCC_PATH")
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
  echo "Warning: no explicit ARM toolchain found; relying on PATH for arm-none-eabi-*" >&2
fi

CPANEL_DIR="$(cd "$SCRIPT_DIR/../cpanel" 2>/dev/null && pwd)"
if [ -d "$CPANEL_DIR" ] && [ -f "$CPANEL_DIR/Makefile" ]; then
  echo "Building cpanel..."
  make -C "$CPANEL_DIR" || exit 1
else
  echo "Warning: cpanel directory not found, using existing cpanel.bin"
fi

SDK_PATH="${PICO_SDK_PATH:-/Users/eositis/pico-sdk}"
if [ ! -d "$SDK_PATH" ]; then
  echo "Error: Pico SDK not found at $SDK_PATH. Set PICO_SDK_PATH to your pico-sdk directory." >&2
  exit 1
fi

# Embed configure-time build id (new values each run → identifiable builds without bumping FIRMWAREVER).
FIRMWARE_BUILD_TIMESTAMP="${FIRMWARE_BUILD_TIMESTAMP:-$(date +%s)}"
FIRMWARE_BUILD_TIMESTAMP_STR="${FIRMWARE_BUILD_TIMESTAMP_STR:-$(date -u +"%Y-%m-%d %H:%M:%S UTC")}"
echo "FIRMWARE_BUILD_TIMESTAMP=$FIRMWARE_BUILD_TIMESTAMP  ($FIRMWARE_BUILD_TIMESTAMP_STR)"
echo "(override either by exporting FIRMWARE_BUILD_TIMESTAMP / FIRMWARE_BUILD_TIMESTAMP_STR before run)"

echo "Configuring pico_release (Pico W)..."
cmake -B pico_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $CMAKE_ARM_TOOLCHAIN

echo "Configuring pico2_release (Pico 2 W)..."
cmake -B pico2_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $CMAKE_ARM_TOOLCHAIN

echo "Building pico_release (-j$JOBS)..."
make -C pico_release -j"$JOBS" || exit 1

echo "Building pico2_release (-j$JOBS)..."
make -C pico2_release -j"$JOBS" || exit 1

echo "OK: pico_release/megaflash.uf2 and pico2_release/megaflash.uf2"

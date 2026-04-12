#!/usr/bin/env bash
# Build Pico W (RP2040) and Pico 2 W (RP2350) **Debug** firmware with Uthernet II / ip65 diagnostics.
# Produces pico_debug/megaflash.uf2 and pico2_debug/megaflash.uf2 (not the same as build-both.sh release).
#
# Enables (via CMakeLists.txt when CMAKE_BUILD_TYPE=Debug):
#   UTHERNET2_DEBUG, U2_ACTIVITY_MONITOR — [u2]/[u2m] UART lines, queued from bus loop safely.
#
# Optional environment (defaults shown):
#   U2_IP65_CHECKPOINT   0     # 0=off, 1=MODE 0x03, 2=RTR0 read, 3=RTR1, 4=RMSR, 5=MACRAW OPEN ok
#   U2_IP65_TRACE_DATA   1     # 1 = log first 48 DATA reads after MR=0x03 ([u2] DATA read addr=…)
#   U2_MON_LOG_BUS       0     # 1 = per-cycle bus log (very verbose; can flood UART)
#   U2_ETH_HEADER_TRACE  0     # 1 = [u2eth] first 64 bytes hex per STA TX/RX frame (UART; not tcpdump)
#
# Examples:
#   ./build-debug-both.sh
#   U2_IP65_CHECKPOINT=2 U2_IP65_TRACE_DATA=0 ./build-debug-both.sh   # bisect RTR0 only, less noise
#
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=build-env.sh
source "$SCRIPT_DIR/build-env.sh"

JOBS="${JOBS:-8}"

U2_IP65_CHECKPOINT="${U2_IP65_CHECKPOINT:-0}"
U2_IP65_TRACE_DATA="${U2_IP65_TRACE_DATA:-1}"
U2_MON_LOG_BUS="${U2_MON_LOG_BUS:-0}"
U2_ETH_HEADER_TRACE="${U2_ETH_HEADER_TRACE:-0}"

# --- ARM toolchain (same as build-both.sh) ---
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
  echo "Error: no usable arm-none-eabi toolchain (see build-both.sh)." >&2
  exit 1
fi

CPANEL_DIR="$(cd "$SCRIPT_DIR/../cpanel" 2>/dev/null && pwd)"
if [ -d "$CPANEL_DIR" ] && [ -f "$CPANEL_DIR/Makefile" ]; then
  echo "Building cpanel (release only)..."
  make -C "$CPANEL_DIR" release || exit 1
else
  echo "Warning: cpanel directory not found, using existing cpanel.bin"
fi

SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"
if [ ! -d "$SDK_PATH" ]; then
  echo "Error: Pico SDK not found at $SDK_PATH. Set PICO_SDK_PATH." >&2
  exit 1
fi

echo "Using CMake: $CMAKE_BIN"
echo "Debug U2 options: U2_IP65_CHECKPOINT=$U2_IP65_CHECKPOINT U2_IP65_TRACE_DATA=$U2_IP65_TRACE_DATA U2_MON_LOG_BUS=$U2_MON_LOG_BUS U2_ETH_HEADER_TRACE=$U2_ETH_HEADER_TRACE"

# Reuse host pioasm from a successful Release configure (same SDK) so Debug builds do not
# regenerate a broken/wrong-architecture pioasm (e.g. "Bad CPU type in executable").
PIOASM_INSTALL_DIR_FLAG=()
if [ -x "$SCRIPT_DIR/pico_release/pioasm-install/pioasm/pioasm" ]; then
  PIOASM_INSTALL_DIR_FLAG=(-DPIOASM_INSTALL_DIR="$SCRIPT_DIR/pico_release/pioasm-install")
  echo "Using pioasm from pico_release/pioasm-install (run ./build-both.sh first if missing)."
else
  echo "Note: pico_release/pioasm-install/pioasm/pioasm not found — CMake will build pioasm; if it fails, run ./build-both.sh then retry."
fi

FIRMWARE_BUILD_TIMESTAMP="${FIRMWARE_BUILD_TIMESTAMP:-$(date +%s)}"
FIRMWARE_BUILD_TIMESTAMP_STR="${FIRMWARE_BUILD_TIMESTAMP_STR:-$(date -u +"%Y-%m-%d %H:%M:%S UTC")}"
echo "FIRMWARE_BUILD_TIMESTAMP=$FIRMWARE_BUILD_TIMESTAMP  ($FIRMWARE_BUILD_TIMESTAMP_STR)"

U2_FLAGS="-DU2_IP65_CHECKPOINT=$U2_IP65_CHECKPOINT -DU2_IP65_TRACE_DATA=$U2_IP65_TRACE_DATA -DU2_MON_LOG_BUS=$U2_MON_LOG_BUS -DU2_ETH_HEADER_TRACE=$U2_ETH_HEADER_TRACE"

echo "Configuring pico_debug (Pico W, Debug)..."
"$CMAKE_BIN" -B pico_debug -S . -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $U2_FLAGS \
  "${PIOASM_INSTALL_DIR_FLAG[@]}" \
  $CMAKE_ARM_TOOLCHAIN

echo "Configuring pico2_debug (Pico 2 W, Debug)..."
"$CMAKE_BIN" -B pico2_debug -S . -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $U2_FLAGS \
  "${PIOASM_INSTALL_DIR_FLAG[@]}" \
  $CMAKE_ARM_TOOLCHAIN

echo "Building pico_debug (-j$JOBS)..."
make -C pico_debug -j"$JOBS" || exit 1

echo "Building pico2_debug (-j$JOBS)..."
make -C pico2_debug -j"$JOBS" || exit 1

echo "OK: pico_debug/megaflash.uf2 and pico2_debug/megaflash.uf2"
echo "Flash the one that matches your board; connect UART 115200 8N1 for [u2]/[u2m] lines during wget65/ip65 init."
echo "For L2/L3/L4 header hex on WiFi STA: U2_ETH_HEADER_TRACE=1 ./build-debug-both.sh → watch [u2eth] TX/RX lines."

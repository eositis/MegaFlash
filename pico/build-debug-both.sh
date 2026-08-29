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
#   U2_MACRAW_COMPAT_DROP_OLDEST 0  # 1 = MACRAW: discard unread RX once when full (compat; helps DHCP bursts)
#
# Examples:
#   ./build-debug-both.sh
#   U2_IP65_CHECKPOINT=2 U2_IP65_TRACE_DATA=0 ./build-debug-both.sh   # bisect RTR0 only, less noise
#
# After a successful build, creates an empty git commit at the MegaFlash repo root
# documenting this run (pico_debug/ and pico2_debug/ are gitignored). Skip: MF_DEBUG_BUILD_NO_GIT_COMMIT=1
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
U2_MACRAW_COMPAT_DROP_OLDEST="${U2_MACRAW_COMPAT_DROP_OLDEST:-0}"

mf_resolve_arm_toolchain || exit 1

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
echo "Debug U2 options: U2_IP65_CHECKPOINT=$U2_IP65_CHECKPOINT U2_IP65_TRACE_DATA=$U2_IP65_TRACE_DATA U2_MON_LOG_BUS=$U2_MON_LOG_BUS U2_ETH_HEADER_TRACE=$U2_ETH_HEADER_TRACE U2_MACRAW_COMPAT_DROP_OLDEST=$U2_MACRAW_COMPAT_DROP_OLDEST"

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

U2_FLAGS="-DU2_IP65_CHECKPOINT=$U2_IP65_CHECKPOINT -DU2_IP65_TRACE_DATA=$U2_IP65_TRACE_DATA -DU2_MON_LOG_BUS=$U2_MON_LOG_BUS -DU2_ETH_HEADER_TRACE=$U2_ETH_HEADER_TRACE -DU2_MACRAW_COMPAT_DROP_OLDEST=$U2_MACRAW_COMPAT_DROP_OLDEST"

echo "Configuring pico_debug (Pico W, Debug, MEGAFLASH_SMB=0)..."
"$CMAKE_BIN" -B pico_debug -S . -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  -DMEGAFLASH_SMB=0 \
  $U2_FLAGS \
  "${PIOASM_INSTALL_DIR_FLAG[@]}" \
  $CMAKE_ARM_TOOLCHAIN

echo "Configuring pico2_debug (Pico 2 W, Debug, MEGAFLASH_SMB=1)..."
"$CMAKE_BIN" -B pico2_debug -S . -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  -DMEGAFLASH_SMB=1 \
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

MF_DEBUG_BUILD_COMMIT_EXTRA="$(printf '%s\n%s\n%s\n%s\n%s\n%s\n' \
  "FIRMWARE_BUILD_TIMESTAMP=$FIRMWARE_BUILD_TIMESTAMP" \
  "FIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  "U2_IP65_CHECKPOINT=$U2_IP65_CHECKPOINT" \
  "U2_IP65_TRACE_DATA=$U2_IP65_TRACE_DATA" \
  "U2_MON_LOG_BUS=$U2_MON_LOG_BUS" \
  "U2_ETH_HEADER_TRACE=$U2_ETH_HEADER_TRACE")"
mf_debug_build_git_commit "pico/build-debug-both.sh"

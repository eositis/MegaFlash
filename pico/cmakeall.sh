#!/bin/bash

# Run from pico directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=build-env.sh
source "$SCRIPT_DIR/build-env.sh"

# Bump version and date for this build (append -eo to denote custom build)
DEFINES="defines.h"
if [ -f "$DEFINES" ]; then
  CURRENT_HEX=$(grep '^#define FIRMWAREVER ' "$DEFINES" | awk '{print $3}' | tr -d '\r\n')
  CURRENT_STR=$(grep '^#define FIRMWAREVERSTR ' "$DEFINES" | sed 's/.*"\(.*\)".*/\1/' | tr -d '\r\n')
  CURRENT_STR="${CURRENT_STR%-eo}"   # strip existing -eo if present
  CURRENT_DEC=$((CURRENT_HEX))
  NEXT_DEC=$((CURRENT_DEC + 1))
  NEXT_VER=$(printf '0x%04x' "$NEXT_DEC")
  # Increment patch (e.g. V1.1.5 -> V1.1.6-eo)
  VER="${CURRENT_STR#V}"
  PATCH="${VER##*.}"
  REST="${VER%.*}"
  [ -z "$REST" ] && REST="$VER" && PATCH="0"
  NEW_PATCH=$((PATCH + 1))
  NEW_VER="V${REST}.${NEW_PATCH}-eo"
  BUILD_DATE=$(date +%d-%b-%Y)
  sed -i.bak "s|^#define FIRMWAREVER .*|#define FIRMWAREVER     $NEXT_VER|" "$DEFINES"
  sed -i.bak "s|^#define FIRMWAREVERSTR .*|#define FIRMWAREVERSTR  \"$NEW_VER\"|" "$DEFINES"
  rm -f "${DEFINES}.bak"
  LAST_VER_LINE=$(grep -n '^// 0x[0-9a-fA-F]* = ' "$DEFINES" | tail -1 | cut -d: -f1)
  if [ -n "$LAST_VER_LINE" ]; then
    sed -i.bak "${LAST_VER_LINE}a\\
// $NEXT_VER = $NEW_VER $BUILD_DATE
" "$DEFINES"
    rm -f "${DEFINES}.bak"
  fi
  echo "Build version: $NEW_VER ($NEXT_VER) $BUILD_DATE"
fi

# Force the ARM toolchain so the same compiler is used (with nosys.specs) and
# macOS uses arm-none-eabi-ranlib, not Xcode's. Prefer explicit path over PATH.
#
# To force a specific toolchain, set ARM_TOOLCHAIN_PATH to its bin directory, e.g.:
#   export ARM_TOOLCHAIN_PATH="/Applications/ArmGNUToolchain/12.3.rel1/arm-none-eabi/bin"
#   ./cmakeall.sh
#
TOOLCHAIN_BIN=""
if [ -n "$ARM_TOOLCHAIN_PATH" ] && mf_try_arm_toolchain_bin "$ARM_TOOLCHAIN_PATH"; then
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

# Build cpanel first (Control Panel binary embedded in firmware)
CPANEL_DIR="$(cd "$SCRIPT_DIR/../cpanel" 2>/dev/null && pwd)"
if [ -d "$CPANEL_DIR" ] && [ -f "$CPANEL_DIR/Makefile" ]; then
  echo "Building cpanel (release only; skips test disk + Java)..."
  make -C "$CPANEL_DIR" release || exit 1
else
  echo "Warning: cpanel directory not found, using existing cpanel.bin"
fi

# PICO_SDK_PATH: use environment if set, else ~/pico-sdk (portable across users / Intel vs ARM macOS)
SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"
if [ ! -d "$SDK_PATH" ]; then
  echo "Error: Pico SDK not found at $SDK_PATH. Set PICO_SDK_PATH or clone: git clone https://github.com/raspberrypi/pico-sdk \"\$HOME/pico-sdk\" && (cd \"\$HOME/pico-sdk\" && git submodule update --init)" >&2
  exit 1
fi

echo "Using CMake: $CMAKE_BIN"

FIRMWARE_BUILD_TIMESTAMP="${FIRMWARE_BUILD_TIMESTAMP:-$(date +%s)}"
FIRMWARE_BUILD_TIMESTAMP_STR="${FIRMWARE_BUILD_TIMESTAMP_STR:-$(date -u +"%Y-%m-%d %H:%M:%S UTC")}"
echo "FIRMWARE_BUILD_TIMESTAMP=$FIRMWARE_BUILD_TIMESTAMP  ($FIRMWARE_BUILD_TIMESTAMP_STR)"

#Pico Build
"$CMAKE_BIN" -B pico_debug   -S . -DCMAKE_BUILD_TYPE=Debug   -DPICO_BOARD=pico_w  -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $CMAKE_ARM_TOOLCHAIN
"$CMAKE_BIN" -B pico_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico_w  -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $CMAKE_ARM_TOOLCHAIN

#Pico2 Build
"$CMAKE_BIN" -B pico2_debug   -S . -DCMAKE_BUILD_TYPE=Debug   -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $CMAKE_ARM_TOOLCHAIN
"$CMAKE_BIN" -B pico2_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" \
  -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" \
  "-DFIRMWARE_BUILD_TIMESTAMP_STR=$FIRMWARE_BUILD_TIMESTAMP_STR" \
  $CMAKE_ARM_TOOLCHAIN

# Build release firmware
make -C pico_release -j4 || exit 1
make -C pico2_release -j4 || exit 1

# Place resultant UF2 files in a folder named with the release version
if [ -n "${NEW_VER:-}" ]; then
  RELEASE_DIR="_releases/$NEW_VER"
  mkdir -p "$RELEASE_DIR"
  cp -f pico_release/megaflash.uf2 "$RELEASE_DIR/megaflash-pico.uf2"
  cp -f pico2_release/megaflash.uf2 "$RELEASE_DIR/megaflash-pico2.uf2"
  # Include CHANGELOG in release dir (default)
  if [ -f "CHANGELOG-NEXT.md" ]; then
    sed "s/@VERSION@/$NEW_VER/g" CHANGELOG-NEXT.md > "$RELEASE_DIR/CHANGELOG.md"
  else
    echo "# $NEW_VER" > "$RELEASE_DIR/CHANGELOG.md"
    echo "" >> "$RELEASE_DIR/CHANGELOG.md"
    echo "*No release notes. Add CHANGELOG-NEXT.md before building for details.*" >> "$RELEASE_DIR/CHANGELOG.md"
  fi
  echo "Release files -> $RELEASE_DIR/"
fi
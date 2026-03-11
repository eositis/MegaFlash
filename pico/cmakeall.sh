#!/bin/bash

# Run from pico directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

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
if [ -n "$ARM_TOOLCHAIN_PATH" ] && [ -x "$ARM_TOOLCHAIN_PATH/arm-none-eabi-gcc" ]; then
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
fi

# Build cpanel first (Control Panel binary embedded in firmware)
CPANEL_DIR="$(cd "$SCRIPT_DIR/../cpanel" 2>/dev/null && pwd)"
if [ -d "$CPANEL_DIR" ] && [ -f "$CPANEL_DIR/Makefile" ]; then
  echo "Building cpanel..."
  make -C "$CPANEL_DIR" || exit 1
else
  echo "Warning: cpanel directory not found, using existing cpanel.bin"
fi

# PICO_SDK_PATH: use environment if set, else default (edit default for your machine)
SDK_PATH="${PICO_SDK_PATH:-/Users/eositis/pico-sdk}"
if [ ! -d "$SDK_PATH" ]; then
  echo "Error: Pico SDK not found at $SDK_PATH. Set PICO_SDK_PATH to your pico-sdk directory." >&2
  exit 1
fi

#Pico Build
cmake -B pico_debug   -S . -DCMAKE_BUILD_TYPE=Debug   -DPICO_BOARD=pico_w  -DPICO_SDK_PATH="$SDK_PATH" $CMAKE_ARM_TOOLCHAIN
cmake -B pico_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico_w  -DPICO_SDK_PATH="$SDK_PATH" $CMAKE_ARM_TOOLCHAIN

#Pico2 Build
cmake -B pico2_debug   -S . -DCMAKE_BUILD_TYPE=Debug   -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" $CMAKE_ARM_TOOLCHAIN
cmake -B pico2_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$SDK_PATH" $CMAKE_ARM_TOOLCHAIN

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
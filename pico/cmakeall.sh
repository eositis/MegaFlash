#!/bin/bash

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
  CMAKE_ARM_TOOLCHAIN="-DCMAKE_C_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-gcc"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_CXX_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-g++"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_AR=$TOOLCHAIN_BIN/arm-none-eabi-ar"
  CMAKE_ARM_TOOLCHAIN="$CMAKE_ARM_TOOLCHAIN -DCMAKE_RANLIB=$TOOLCHAIN_BIN/arm-none-eabi-ranlib"
  echo "Using ARM toolchain: $TOOLCHAIN_BIN"
fi

#Pico Build
cmake -B pico_debug   -S . -DCMAKE_BUILD_TYPE=Debug   -DPICO_BOARD=pico_w $CMAKE_ARM_TOOLCHAIN
cmake -B pico_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico_w $CMAKE_ARM_TOOLCHAIN

#Pico2 Build
cmake -B pico2_debug   -S . -DCMAKE_BUILD_TYPE=Debug   -DPICO_BOARD=pico2_w $CMAKE_ARM_TOOLCHAIN
cmake -B pico2_release -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2_w $CMAKE_ARM_TOOLCHAIN
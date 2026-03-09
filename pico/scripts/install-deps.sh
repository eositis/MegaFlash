#!/bin/bash
# Install dependencies for building MegaFlash Pico firmware on macOS.
# Run from repo root or pico directory.
#
# One-time prerequisite (run in Terminal; requires your password):
#   sudo xcodebuild -license accept
#
# Then run this script:
#   ./pico/scripts/install-deps.sh
# or from pico dir:
#   ./scripts/install-deps.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PICO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PICO_DIR"

echo "=== MegaFlash Pico build dependencies (macOS) ==="

# Check for Xcode license (brew installs often fail if not accepted)
if ! xcodebuild -version &>/dev/null; then
  echo "Xcode / Command Line Tools may not be set up or license not accepted."
  echo "Run once:  sudo xcodebuild -license accept"
  echo "Then re-run this script."
  exit 1
fi

# Homebrew
if ! command -v brew &>/dev/null; then
  echo "Homebrew not found. Install from https://brew.sh then re-run this script."
  exit 1
fi

echo "Installing CMake and ARM GCC toolchain (this may take a few minutes)..."
brew install cmake
brew install arm-none-eabi-gcc

echo ""
echo "Verifying..."
command -v cmake
command -v arm-none-eabi-gcc
arm-none-eabi-gcc --version | head -1

echo ""
echo "Pico SDK: cmakeall.sh expects the SDK at: ${PICO_SDK_PATH:-/Users/eositis/pico-sdk}"
if [ -d "${PICO_SDK_PATH:-/Users/eositis/pico-sdk}" ]; then
  echo "  Found."
else
  echo "  Not found. To clone the Pico SDK:"
  echo "    export PICO_SDK_PATH=\$HOME/pico-sdk"
  echo "    git clone --depth 1 --branch 1.5.1 https://github.com/raspberrypi/pico-sdk.git \$PICO_SDK_PATH"
  echo "    cd \$PICO_SDK_PATH && git submodule update --init"
  echo "  Then set PICO_SDK_PATH in your shell before running cmakeall.sh."
fi

echo ""
echo "Done. Build with:  ./cmakeall.sh"

#!/usr/bin/env bash
# Build **Pico 2 W** Debug firmware (RP2350; ip65 UART diagnostics). Does not bump defines.h.
# Configure once, e.g.:
#   cmake -B pico2_debug -S . -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug
# Optional bisect: -DU2_IP65_CHECKPOINT=3  (see debug/README.md, CMakeLists U2_* cache vars)
#
#   ./build-debug.sh
#   JOBS=8 ./build-debug.sh
#
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=build-env.sh
source "$SCRIPT_DIR/build-env.sh"
JOBS="${JOBS:-8}"

if [ ! -f pico2_debug/Makefile ]; then
  echo "Missing pico2_debug/Makefile — configure with the same CMake you use for release, e.g.:" >&2
  echo "  \"$CMAKE_BIN\" -B pico2_debug -S . -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug" >&2
  exit 1
fi

echo "Building pico2_debug (Pico 2 W / RP2350) with $CMAKE_BIN..."
"$CMAKE_BIN" --build pico2_debug -j"$JOBS"
echo "OK: pico2_debug/megaflash.uf2 — flash this for MegaFlash + ip65 bring-up."
echo "UART 115200; default [u2] is checkpoint-only (U2_IP65_CHECKPOINT). Use -DU2_IP65_TRACE_DATA=1 for 48 DATA lines."

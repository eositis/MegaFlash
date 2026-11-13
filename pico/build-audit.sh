#!/usr/bin/env bash
# Build the section 1ev bus-pipeline AUDIT image (U2_BUS_AUDIT=1) for Pico 2 W (RP2350) into
# pico2_audit/, leaving pico2_release/ untouched. The audit image streams [ba] lines on UART0
# (GPIO 0/1, 115200 8N1 by default; USB cannot be used while the Apple is attached).
# Optional: BOARDS="pico2_w pico_w" ./build-audit.sh   (RP2040 has little spare RAM; pico2 only by default)
#           U2_BUS_AUDIT_BAUD=460800 ./build-audit.sh
# After a successful build the UF2 is also copied to audit_images/ with its build stamp so a
# later rebuild cannot overwrite it.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=build-env.sh
source "$SCRIPT_DIR/build-env.sh"
JOBS="${JOBS:-8}"
BOARDS="${BOARDS:-pico2_w}"
BAUD="${U2_BUS_AUDIT_BAUD:-115200}"

TOOLCHAIN_BIN=""
if [ -n "${ARM_TOOLCHAIN_PATH:-}" ] && mf_try_arm_toolchain_bin "$ARM_TOOLCHAIN_PATH"; then
  TOOLCHAIN_BIN="$ARM_TOOLCHAIN_PATH"
fi
if [ -z "$TOOLCHAIN_BIN" ]; then
  echo "Set ARM_TOOLCHAIN_PATH to an Arm GNU toolchain bin dir (see build-both.sh)." >&2
  exit 1
fi
export PATH="$TOOLCHAIN_BIN:$PATH"
TC="-DPICO_TOOLCHAIN_PATH=$TOOLCHAIN_BIN -DCMAKE_C_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-gcc -DCMAKE_CXX_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-g++ -DCMAKE_ASM_COMPILER=$TOOLCHAIN_BIN/arm-none-eabi-gcc -DCMAKE_AR=$TOOLCHAIN_BIN/arm-none-eabi-ar -DCMAKE_RANLIB=$TOOLCHAIN_BIN/arm-none-eabi-ranlib -DCMAKE_OBJDUMP=$TOOLCHAIN_BIN/arm-none-eabi-objdump -DCMAKE_OBJCOPY=$TOOLCHAIN_BIN/arm-none-eabi-objcopy"
SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"

CPANEL_DIR="$(cd "$SCRIPT_DIR/../cpanel" 2>/dev/null && pwd)"
if [ -d "$CPANEL_DIR" ] && [ -f "$CPANEL_DIR/Makefile" ]; then
  make -C "$CPANEL_DIR" release || exit 1
fi

mf_firmware_build_stamp
STAMP_STR="$FIRMWARE_BUILD_TIMESTAMP_STR AUDIT"
mkdir -p audit_images
for BOARD in $BOARDS; do
  case "$BOARD" in pico_w) DIR=pico_audit ;; pico2_w) DIR=pico2_audit ;; *) echo "bad board $BOARD" >&2; exit 1 ;; esac
  echo "Configuring $DIR ($BOARD, U2_BUS_AUDIT=1)..."
  # shellcheck disable=SC2086
  "$CMAKE_BIN" -B "$DIR" -S . -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD="$BOARD" -DPICO_SDK_PATH="$SDK_PATH" \
    -DFIRMWARE_BUILD_TIMESTAMP="$FIRMWARE_BUILD_TIMESTAMP" "-DFIRMWARE_BUILD_TIMESTAMP_STR=$STAMP_STR" \
    -DU2_BUS_AUDIT=1 -DU2_BUS_AUDIT_BAUD="$BAUD" $TC
  make -C "$DIR" -j"$JOBS" || exit 1
  if [ "$BOARD" = pico2_w ]; then
    python3 "$SCRIPT_DIR/tools/check_bus_ram.py" "$DIR/megaflash.elf" --objdump "$TOOLCHAIN_BIN/arm-none-eabi-objdump" || \
      echo "NOTE: bus call tree reaches flash in this image (expected for a pre-section-1ev baseline)."
  fi
  OUT="audit_images/megaflash-${BOARD}-$(echo "$FIRMWARE_BUILD_TIMESTAMP_STR" | sed 's/.* \(b[0-9]*\)$/\1/')-audit.uf2"
  cp "$DIR/megaflash.uf2" "$OUT"
  echo "OK: $DIR/megaflash.uf2  (copy: $OUT)"
done

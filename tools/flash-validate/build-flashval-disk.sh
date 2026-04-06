#!/usr/bin/env bash
# Build 800K ProDOS disk FLASHVALID.dsk: PRODOS + BASIC.SYSTEM from pico/romdisk.po,
# tokenized FLASHVAL (screen suite), and FLASHVAL.SRC (full listing as TXT).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROMDISK="${ROMDISK:-$REPO_ROOT/pico/romdisk.po}"
OUT="${OUT:-$SCRIPT_DIR/FLASHVALID.dsk}"
DSK_BAS="$SCRIPT_DIR/FLASHVAL.DSK.BAS"
FULL_BAS="$SCRIPT_DIR/FLASHVAL.BAS"

if [[ ! -f "$ROMDISK" ]]; then
  echo "Missing romdisk image: $ROMDISK" >&2
  echo "Set ROMDISK=path/to/romdisk.po or add pico/romdisk.po to the repo build." >&2
  exit 1
fi

AC_JAR="${AC_JAR:-}"
if [[ -z "$AC_JAR" ]]; then
  for c in \
    "$HOME/Library/Application Support/AppleCommander/AppleCommander-ac-13.0.jar" \
    "$HOME/Library/Application Support/AppleCommander/ac.jar"
  do
    if [[ -f "$c" ]]; then
      AC_JAR="$c"
      break
    fi
  done
fi
if [[ -z "$AC_JAR" || ! -f "$AC_JAR" ]]; then
  echo "Set AC_JAR to AppleCommander-ac-*.jar (e.g. from https://applecommander.github.io/)." >&2
  exit 1
fi

java_cmd=(java -jar "$AC_JAR")
WRK="$(mktemp -d)"
cleanup() { rm -rf "$WRK"; }
trap cleanup EXIT

# AppleCommander expects 800K images for this romdisk layout; pad with zeros.
dd if=/dev/zero of="$WRK/rom800.po" bs=819200 count=1 2>/dev/null
dd if="$ROMDISK" of="$WRK/rom800.po" conv=notrunc 2>/dev/null

"${java_cmd[@]}" -g "$WRK/rom800.po" PRODOS "$WRK/PRODOS.bin"
"${java_cmd[@]}" -g "$WRK/rom800.po" BASIC.SYSTEM "$WRK/BASIC.SYSTEM.bin"

"${java_cmd[@]}" -pro800 "$OUT" FLASHVALID
"${java_cmd[@]}" -p "$OUT" PRODOS SYS < "$WRK/PRODOS.bin"
"${java_cmd[@]}" -p "$OUT" BASIC.SYSTEM SYS < "$WRK/BASIC.SYSTEM.bin"
"${java_cmd[@]}" -bas "$OUT" FLASHVAL < "$DSK_BAS"
"${java_cmd[@]}" -ptx "$OUT" FLASHVAL.SRC < "$FULL_BAS"

echo "Wrote $OUT"

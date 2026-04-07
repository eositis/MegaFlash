#!/usr/bin/env bash
# Build a bootable ProDOS 140K disk for flash validation tools.
# Mechanism matches ../a2speed/Makefile `disk` target: -pro140, then AppleCommander -p/-bas/-ptx.
# System files come from cpanel/prodos19.dsk (standard 143360-byte ProDOS 1.9 image in this repo),
# not from padding pico/romdisk.po to 800K (that produced images many tools reject).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT="${OUT:-$SCRIPT_DIR/FLASHVALID.po}"
SYS_SRC="${SYS_SRC:-$REPO_ROOT/cpanel/prodos19.dsk}"
DSK_BAS="$SCRIPT_DIR/FLASHVAL.DSK.BAS"
FULL_BAS="$SCRIPT_DIR/FLASHVAL.BAS"
SOAK_BAS="$SCRIPT_DIR/FLASHSOAK.BAS"
VOL_NAME="${VOL_NAME:-FLASHVALID}"

# Java: same preference as a2speed/Makefile (arm64 Homebrew OpenJDK avoids "Bad CPU type" on Apple Silicon).
if [[ -n "${JAVA:-}" ]]; then
  :
elif [[ -x "/opt/homebrew/opt/openjdk/bin/java" ]]; then
  JAVA="/opt/homebrew/opt/openjdk/bin/java"
else
  JAVA="java"
fi

# AppleCommander jar: default like a2speed (AppleCommander-ac.jar), then common fallbacks.
AC_JAR="${AC_JAR:-}"
if [[ -z "$AC_JAR" ]]; then
  for c in \
    "$HOME/Library/Application Support/AppleCommander/AppleCommander-ac.jar" \
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
  echo "Set AC_JAR to AppleCommander-ac.jar (e.g. from https://applecommander.github.io/)." >&2
  exit 1
fi

if [[ ! -f "$SYS_SRC" ]]; then
  echo "Missing system-file source image: $SYS_SRC" >&2
  echo "Expected cpanel/prodos19.dsk in the MegaFlash tree." >&2
  exit 1
fi

java_cmd=("$JAVA" -jar "$AC_JAR")
WRK="$(mktemp -d)"
cleanup() { rm -rf "$WRK"; }
trap cleanup EXIT

"${java_cmd[@]}" -g "$SYS_SRC" PRODOS "$WRK/PRODOS.bin"
"${java_cmd[@]}" -g "$SYS_SRC" BASIC.SYSTEM "$WRK/BASIC.SYSTEM.bin"

echo "Creating ProDOS 140K image $OUT (volume /$VOL_NAME/)..."
"${java_cmd[@]}" -pro140 "$OUT" "$VOL_NAME"

"${java_cmd[@]}" -p "$OUT" PRODOS SYS < "$WRK/PRODOS.bin"
"${java_cmd[@]}" -p "$OUT" BASIC.SYSTEM SYS < "$WRK/BASIC.SYSTEM.bin"
"${java_cmd[@]}" -bas "$OUT" FLASHVAL < "$DSK_BAS"
"${java_cmd[@]}" -bas "$OUT" FLASHSOAK < "$SOAK_BAS"
"${java_cmd[@]}" -ptx "$OUT" FLASHVAL.SRC < "$FULL_BAS"
"${java_cmd[@]}" -ptx "$OUT" FLASHSOAK.SRC < "$SOAK_BAS"

echo "Wrote $OUT"

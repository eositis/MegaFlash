#!/bin/bash
# Rewrites --specs=nosys.specs to use bundled scripts/nosys.specs (Homebrew arm-none-eabi-gcc lacks it).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NOSYS="$SCRIPT_DIR/nosys.specs"
REAL_GCC="${MEGAFLASH_REAL_CC:-arm-none-eabi-gcc}"
args=()
for a in "$@"; do
  if [ "$a" = "--specs=nosys.specs" ] || [ "$a" = "-specs=nosys.specs" ]; then
    [ -f "$NOSYS" ] && args+=("-specs=$NOSYS") || args+=("$a")
  else
    args+=("$a")
  fi
done
exec "$REAL_GCC" "${args[@]}"

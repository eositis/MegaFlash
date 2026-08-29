# Sourced by pico/*.sh after `cd` to the pico directory.
#
# Host CMake (arm64 vs x86_64): On Apple Silicon, PATH often still finds an Intel
# Homebrew or old /usr/local/cmake first → "bad CPU type in executable". Prefer
# Apple Silicon Homebrew's cmake, then Intel prefix, then PATH.
#
# Override: export CMAKE=/opt/homebrew/bin/cmake

_mf_cmake="${CMAKE:-}"
CMAKE_BIN=""
if [ -n "$_mf_cmake" ]; then
  if [ -x "$_mf_cmake" ]; then
    CMAKE_BIN="$_mf_cmake"
  elif command -v "$_mf_cmake" >/dev/null 2>&1; then
    CMAKE_BIN=$(command -v "$_mf_cmake")
  fi
fi
if [ -z "$CMAKE_BIN" ]; then
  for _c in /opt/homebrew/bin/cmake /usr/local/bin/cmake; do
    if [ -x "$_c" ]; then
      CMAKE_BIN="$_c"
      break
    fi
  done
fi
if [ -z "$CMAKE_BIN" ]; then
  CMAKE_BIN=$(command -v cmake 2>/dev/null || true)
fi
CMAKE_BIN="${CMAKE_BIN:-cmake}"
unset _mf_cmake _c

# True if arm-none-eabi-gcc in $1 runs on this host AND Pico SDK can link (newlib nosys.specs).
# Skips: Intel-only Arm .pkg on Apple Silicon; Homebrew formula arm-none-eabi-gcc (GCC without newlib).
# Accepts: Homebrew cask gcc-arm-embedded (shims in /opt/homebrew/bin with nosys.specs).
mf_try_arm_toolchain_bin() {
  local d="$1"
  local specs
  [ -n "$d" ] || return 1
  [ -x "$d/arm-none-eabi-gcc" ] || return 1
  "$d/arm-none-eabi-gcc" --version >/dev/null 2>&1 || return 1
  specs=$("$d/arm-none-eabi-gcc" -print-file-name=nosys.specs 2>/dev/null || true)
  [ -n "$specs" ] && [ -f "$specs" ]
}

# Set TOOLCHAIN_BIN and CMAKE_ARM_TOOLCHAIN. Return 1 if none (already printed).
# Order: ARM_TOOLCHAIN_PATH override, then Homebrew prefixes, then Arm .pkg under
# /Applications, then PATH. Homebrew cask gcc-arm-embedded is the supported macOS install.
mf_resolve_arm_toolchain() {
  TOOLCHAIN_BIN=""
  CMAKE_ARM_TOOLCHAIN=""

  if [ -n "${ARM_TOOLCHAIN_PATH:-}" ] && mf_try_arm_toolchain_bin "$ARM_TOOLCHAIN_PATH"; then
    TOOLCHAIN_BIN="$ARM_TOOLCHAIN_PATH"
  fi
  if [ -z "$TOOLCHAIN_BIN" ]; then
    local _hb
    for _hb in /opt/homebrew /usr/local; do
      if mf_try_arm_toolchain_bin "$_hb/bin"; then
        TOOLCHAIN_BIN="$_hb/bin"
        break
      fi
    done
  fi
  if [ -z "$TOOLCHAIN_BIN" ] && [ -d /Applications/ArmGNUToolchain ]; then
    local d
    for d in /Applications/ArmGNUToolchain/*/arm-none-eabi/bin; do
      if mf_try_arm_toolchain_bin "$d"; then
        TOOLCHAIN_BIN="$d"
        break
      fi
    done
  fi
  if [ -z "$TOOLCHAIN_BIN" ]; then
    local GCC_PATH _gdir
    GCC_PATH=$(command -v arm-none-eabi-gcc 2>/dev/null || true)
    if [ -n "$GCC_PATH" ]; then
      _gdir=$(dirname "$GCC_PATH")
      if mf_try_arm_toolchain_bin "$_gdir"; then
        TOOLCHAIN_BIN="$_gdir"
      fi
    fi
  fi

  if [ -z "$TOOLCHAIN_BIN" ]; then
    echo "Error: no usable arm-none-eabi toolchain (need host-native GCC + newlib, e.g. nosys.specs)." >&2
    echo "  macOS: brew install --cask gcc-arm-embedded   # full Arm GNU Toolchain; needs admin for the .pkg" >&2
    echo "  Also:  brew install cmake" >&2
    echo "  Not enough: brew install arm-none-eabi-gcc (formula, no newlib)." >&2
    echo "  Override: export ARM_TOOLCHAIN_PATH to a bin directory that has nosys.specs." >&2
    return 1
  fi

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
  return 0
}

# After a successful debug firmware build: create a marker commit at the repo root.
# Artifacts under pico_debug/ and pico2_debug/ are gitignored; --allow-empty still
# records when and with which CMake env the build ran.
#
# Skip entirely: MF_DEBUG_BUILD_NO_GIT_COMMIT=1
# Optional extra lines (key=value or free text): MF_DEBUG_BUILD_COMMIT_EXTRA
mf_debug_build_git_commit() {
  [ -z "${MF_DEBUG_BUILD_NO_GIT_COMMIT:-}" ] || return 0
  local label="${1:-debug build}"
  local repo head branch dirty
  repo=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "Note: not a git repository — skipping debug-build git commit."
    return 0
  }
  head=$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo "?")
  branch=$(git -C "$repo" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
  if git -C "$repo" diff-index --quiet HEAD -- 2>/dev/null; then
    dirty=clean
  else
    dirty=dirty
  fi

  local msgf
  msgf=$(mktemp "${TMPDIR:-/tmp}/mf-debug-build-commit.XXXXXX") || return 0
  {
    printf '%s\n\n' "debug build: $label"
    printf 'HEAD=%s branch=%s tree=%s\n' "$head" "$branch" "$dirty"
    printf 'host=%s\n' "$(uname -srm 2>/dev/null || true)"
    if [ -n "${MF_DEBUG_BUILD_COMMIT_EXTRA:-}" ]; then
      printf '\n%s\n' "$MF_DEBUG_BUILD_COMMIT_EXTRA"
    fi
  } >"$msgf"

  if git -C "$repo" commit --allow-empty -F "$msgf"; then
    echo "Git: recorded debug build marker at $(git -C "$repo" rev-parse --short HEAD) ($repo)"
  else
    echo "Warning: debug-build git commit failed (configure user.name/user.email, or check git hooks)." >&2
  fi
  rm -f "$msgf"
}

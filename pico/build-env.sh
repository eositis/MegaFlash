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
# Skips: Intel-only Arm .pkg on Apple Silicon; Homebrew arm-none-eabi-gcc (GCC without newlib).
mf_try_arm_toolchain_bin() {
  local d="$1"
  local specs
  [ -n "$d" ] || return 1
  [ -x "$d/arm-none-eabi-gcc" ] || return 1
  "$d/arm-none-eabi-gcc" --version >/dev/null 2>&1 || return 1
  specs=$("$d/arm-none-eabi-gcc" -print-file-name=nosys.specs 2>/dev/null || true)
  [ -n "$specs" ] && [ -f "$specs" ]
}

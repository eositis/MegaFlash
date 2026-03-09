# Pico Firmware

## Build Environment

The software is compiled on Linux or macOS. You need CMake, the ARM GCC toolchain (`arm-none-eabi-gcc`), and the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk).

### macOS: install dependencies

1. **One-time:** Accept the Xcode license (required for Homebrew installs):
   ```bash
   sudo xcodebuild -license accept
   ```
2. Install build tools and ARM toolchain:
   ```bash
   ./scripts/install-deps.sh
   ```
   This uses Homebrew to install `cmake` and `arm-none-eabi-gcc`. If the Pico SDK is not yet at `$HOME/pico-sdk` (or `PICO_SDK_PATH`), the script prints instructions to clone it.

   **Note:** Homebrew’s `arm-none-eabi-gcc` is built without newlib headers (`stdio.h`, `assert.h`, etc.), so the main firmware build will fail with “No such file or directory” for standard headers. For a full build on macOS, use the [official ARM GNU toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (macOS package or tarball). After installing, set `ARM_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` directory, e.g.:
   - Package install: `export ARM_TOOLCHAIN_PATH="/Applications/ArmGNUToolchain/15.2.Rel1/arm-none-eabi/bin"`
   - Tarball in home: `export ARM_TOOLCHAIN_PATH="$HOME/arm-gnu-toolchain/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi/bin"`
   Then run `./cmakeall.sh` from the `pico` directory. The project includes a bundled `scripts/nosys.specs` for toolchains that lack it; you can copy it into the toolchain’s lib dir if needed (see implementation notes).

3. **Pico SDK:** If you don’t have the SDK, clone it and set `PICO_SDK_PATH` before building:
   ```bash
   export PICO_SDK_PATH=$HOME/pico-sdk
   git clone --depth 1 --branch 1.5.1 https://github.com/raspberrypi/pico-sdk.git $PICO_SDK_PATH
   cd $PICO_SDK_PATH && git submodule update --init
   ```
   Then run `./cmakeall.sh` from the `pico` directory.

Go to the MegaFlash `pico` source directory. Execute the shell script file by

```
./cmakeall.sh
```
If everything is correct, the following sub-directories should be created.

```
pico_debug
pico_release
pico2_debug
pico2_release
picotool
```

Note: You need to execute the shell script only once unless `CMakeLists.txt` file is changed or you want to recreate the build directories.

`pico_debug` and `pico_release` are the build directories for Pico W (RP2040). `pico2_debug` and `pico2_release` are for Pico 2 W (RP2350); they are built when the SDK has `pico2_w` support.

## Build Instruction

Before compiling the pico firmware, the control panel must be built first. Please follow the instruction in `cpanel` directory to build the control panel binary.

To build the pico firmware, go to one of the build directory e.g. `pico2_release`. Then, execute `make`. The output file is `megaflash.uf2`.




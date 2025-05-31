# Pico Firmware

## Build Environment

Download and setup Raspberry Pi PICO C/C++ SDK from [github](https://github.com/raspberrypi/pico-sdk "github"). Make sure `PICO_SDK_PATH` variable is set.

Go to the MegaFlash `pico` source directory. Execute the shell script file by

```
./cmakeall.sh
```
If everything is correct, 5 directories should be created.

```
pico_debug
pico_release
pico2_debug
pico2_release
picotool
```

Note: You need to execute the shell script only once unless `CMakeLists.txt` file is changed or you want to recreate the build directories.

`pico_debug` and `pico_release` are the build directories for Pico Board (RP2040).  `pico2_debug` and `pico2_release` are the build directories for Pico2 Board (RP2350).

## Build Instruction

Before compiling the pico firmware, the control panel must be built first. Please follow the instruction in `cpanel` directory to build the control panel binary.

To build the pico firmware, go to one of the build directory e.g. `pico2_release`. Then, execute `make`. The output file is `megaflash.uf2`.




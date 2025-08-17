# Control Panel

Control Panel is a sub-project of MegaFlash. Since there is not enough space to store the control panel program in the ROM, the control panel program is stored in Pico.  To execute the control panel program, the program code is copied to Apple RAM memory and executed from there.

## Build Environment

The project is built on Linux with CC65 compiler.  Follow [the instruction](https://cc65.github.io/getting-started.html) to compile CC65. Make sure to add cc65 binaries directory to PATH variable by running 

```sudo make avail```

Java runtime is also needed to build the test target.  Check if Java is installed by running 

```java -version```

If it is not installed, try to install Java Runtime Environment by

```sudo apt install default-jre```

## Build Instruction

Execute `make release` to build the project.

## Build Targets

There are two build targets, `release` and `test`.

The output file of release build is `cpanel.bin`. This file is included to Pico firmware.  

The test build generates a ProDOS executable for testing under emulator. The output file of test build is `cpanel.as`, which is in AppleSingle format. This file is imported to `prodos19.img` disk image file automatically during the build process. The control panel program can be tested on emulator simply by mounting `prodos19.img` image file to emulated Apple.



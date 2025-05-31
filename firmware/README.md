# Apple Firmware Patches

MegaFlash is an internal storage device for Apple IIc/IIc plus computer.  Unlike Apple IIe, address $C100-$CFFF is solely used by internal ROM only.  The only way to make MegaFlash work is to patch the system ROM directly.

The virtual slot of Apple IIc is assigned as follows.  

```
Slot 1 - Serial Port 1
Slot 2 - Serial Port 2
Slot 3 - 80 Column Display
Slot 4 - Apple Memory Expansion Card (aka Slinky)
Slot 5 - 3.5" floppy drive/external Smartport
Slot 6 - 5.25" floppy drive
Slot 7 - Mouse
```

MegaFlash needs to reside in one of the virtual slots. Since MegaFlash is plugged into the Memory Expansion slot, slot 4 is the obvious choice. The Memory Expansion card firmware is removed and replaced by our codes.


## Build Environment

The project is built on Linux with CA65 assembler (part of CC65 compiler toolchain).  Follow [the instruction](https://cc65.github.io/getting-started.html) to compile CC65. Make sure to add cc65 binaries directory to PATH variable by running 

```sudo make avail```

## Original ROM files

Original ROM image files for Apple IIc and IIc plus are needed.  For Apple IIc, ROM Version 4 is needed.  There is only one ROM release (Version 5) for Apple IIc plus. The ROM files can be downloaded from Internet. Name the files `rom4.bin` and `rom5.bin` respectively and put them to the source directory.

## Build Instruction

Execute `make` to build the project.

## Build Target

There are two build targets, `a2c` and `a2cp`.

Target `a2c` is for Apple IIc. The output file is `iic.bin`. Target `a2cp` is for Apple IIc plus. The output file is `iicplus.bin`.

The linker of CC65 is capable of patching a binary file. So, no additional tools are needed to generate the output file.

## Development Guide
The makefile generates the output files in two steps. Step 1 generates the new binary codes. Step 2 merges the new codes to the orginal ROM files.

### Step 1 Code Generation

To add a new code segment, you need to edit `iic.cfg` and `iicplus.cfg` files. A new segment and a new output file should be added.  The naming of segment and output file is same as ROM4X project. i.e. `B0_C123` means address $C123 at ROM Bank 0.

### Step 2 Patching the ROM files

To incorporate the new codes to the ROM files, you need to edit `merge_iic.cfg` and `merge_iic.s` files (`merge_iicp.cfg` and `merge_iicp.s` for Apple IIc plus).

`merge_iic.cfg` tells the linker the address of code segment. `merge_iic.s` uses `.incbin` directive command to include the binary code file.



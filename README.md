# MegaFlash for Apple IIc/IIc+

MegaFlash is an internal storage device for Apple IIc/IIc plus computer.

## Feature List

- 128MB/256MB Capacity, divided into 4/8 ProDOS Drives
- 400kB RAM Drive
- ROM Disk for Disaster Recovery
- 256kB Slinky Emulation on stock firmware
- Boot Menu (Similar to ROM4X/5X)
- Control Panel
- Real Time Clock with ProDOS clock driver
- Network Time Sync (NTP Client)
- Upload/Download ProDOS Image file via WIFI and TFTP Server
- Upload/Download ProDOS Image file by XModem
- FPU for Applesoft BASIC
- Bug fixes of System ROM and Applesoft

## Requirement
- Apple IIc computer with Memory Expansion Card connector or
- Apple IIc plus computer

System ROM replacement is required for MegaFlash to function.

## Hardware
The hardware is quite simple.  All the hardwork is handled by Pi Pico.  Apple 6502 CPU only needs to copy data from/to Pico.

- Raspberry Pi Pico2 W
- Winbond 25Q01 NOR Flash memory
- GAL16V8 PLD for address decode
- 5V to 3.3V Logic Level Shifter


## Project Directories

There are 3 software projects. There is not enough space in Apple ROM to store the Control Panel program.  The program is stored in Pico flash memory. 

`firmware` Project Directory of Apple ROM Patches\
`cpanel` Control Panel project\
`pico` Raspberry Pi Pico firmware\
`common` Common header files used by all 3 projects



# MegaFlash for Apple IIc/IIc+

MegaFlash is an internal storage device for Apple IIc/IIc plus computer.

![MegaFlash-New](https://github.com/user-attachments/assets/00a88a9c-9fd9-48da-8e65-8fe63ef57444)

## Feature List

- 128MB/256MB Capacity, divided into 4/8 ProDOS Drives
- 256kB RAM Disk on Pico 2 (RP2350); 140kB on Pico W (RP2040)
- ROM Disk for Disaster Recovery
- 256kB Slinky Emulation on stock firmware
- Boot Menu (Similar to ROM4X/5X)
- Control Panel
- Real Time Clock with ProDOS clock driver
- Network Time Sync (NTP Client)
- Upload/Download ProDOS Image file via WIFI and TFTP Server
- Upload/Download ProDOS Image file via XModem and USB serial port
- FPU for Applesoft BASIC
- Bug fixes of System ROM and Applesoft

A brief [user guide](https://github.com/eositis/MegaFlash/blob/main/MegaFlash%20for%20a2s4000%20installation%20guide.pdf) is available for more information.

## Requirement
- Apple IIc computer with Memory Expansion Card connector or
- Apple IIc plus computer

System ROM replacement is required for MegaFlash to function.

## Hardware
The hardware is quite simple.  All the hard work is handled by Pi Pico.  Apple 65C02 CPU only needs to copy data between RAM and Pico. Schematic is available [here](./kicad/rev1.0/schematic-rev1.0.pdf).

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

## Sponsorship

MegaFlash development for commercial sale has been partially sponsored by PCBway. Preparing products for commercial release is expensive, often requiring board respins to resolve manufacturing and logistics issues. PCBway production and delivery are fast and efficient, helping compress the go-to market timeline. Many thanks!

<p align="center">
  <img src="[https://example.com](https://pcbwayfile.s3-us-west-2.amazonaws.com/project/21/05/05/0141152434522.jpg
)" alt="PCBway logo">
</p>

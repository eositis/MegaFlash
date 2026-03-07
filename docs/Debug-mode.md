# MegaFlash Debug Mode

MegaFlash has **two separate debug mechanisms**: one in the **Apple II ROM firmware** (6502) and one in the **Pico firmware** (C/C++). They use different outputs and build flags.

---

## 1. Firmware (Apple ROM) debug – Serial Port 1

### What it is

The **firmware** (ROM patches in the `firmware/` folder) can print SmartPort and ProDOS call information to the Apple IIc’s **Serial Port 1** (ACIA at $C098–$C09B). This is intended for debugging driver and ProDOS interaction from the Apple side.

### How to enable

In **`firmware/buildflags.inc`** set:

```asm
DEBUG   EQU   TRUE
```

Default is **FALSE**. Then rebuild the ROM (`make a2c` / `make a2cp`) to get `iic.bin` or `iicplus.bin` with debug enabled.

### What it does

- **Cold start** (`megaflash.s`): Initializes ACIA 1 for debug output (e.g. 19200 baud, no interrupt). Clears IRQ status.
- **`print` routine** (IOROM): Sends one character in A to **acia1data** ($C098), waiting on **acia1status** ($C099) for the transmitter. If bits 5 and 6 of status are set (e.g. MAME without bit-banger), it skips printing so the code doesn’t hang.
- **SmartPort debug** (`smartport.s`):
  - **prnspparam**: Prints `S` + command letter + parameter bytes for SmartPort calls.
  - **prnpdparam**: Prints `P` + ProDOS command letter + unit, buffer address, block number for ProDOS block calls.
  - **prnerr**: Prints `:` + error code + CR/LF.
  - Uses a small command/parameter table for single-letter abbreviations (e.g. S/R/W/F for Status/Read/Write/Format).
- **Conditional code**: All of the above is wrapped in `.if DEBUG` / `.endif`, so with `DEBUG EQU FALSE` it is not assembled into the ROM.

### Output

Debug text goes out over **Serial Port 1** (slot 1). You need a serial terminal or cable on the IIc’s serial port (e.g. modem/printer port) at the baud rate set in the init (e.g. 19200) to see it.

---

## 2. Pico firmware debug – UART / USB

### What it is

The **Pico firmware** (`pico/`) can print diagnostic messages to the Pico’s **stdio** (UART or USB serial). This is controlled by the **build type** (Debug vs Release), not a separate “debug mode” flag at runtime.

### How to enable

Build the Pico firmware as **Debug**:

- Use a **Debug** build directory, e.g. `pico_debug` or `pico2_debug` (from `cmakeall.sh` with `-DCMAKE_BUILD_TYPE=Debug`).
- So: **Debug build** = debug output enabled; **Release build** = debug output disabled.

The C preprocessor symbol **`NDEBUG`** is defined in **Release** builds. In **Debug** builds `NDEBUG` is not defined, so the macros in **`pico/debug.h`** expand to real `printf` calls.

### Log levels (`pico/debug.h`)

`debug.h` defines levels and macros that compile to nothing when `NDEBUG` is set, or to `printf(...)` when it is not:

| Level   | Macro          | Typical use      |
|---------|----------------|------------------|
| ERROR   | ERROR_PRINTF   | Errors           |
| WARN    | WARN_PRINTF    | Warnings         |
| INFO    | INFO_PRINTF    | Important events |
| DEBUG   | DEBUG_PRINTF   | General debug    |
| TRACE   | TRACE_PRINTF   | Fine-grained     |

**LOG_LEVEL** is set to **LEVEL_DEBUG**, so ERROR, WARN, INFO, and DEBUG are active in Debug builds; TRACE can be enabled by lowering LOG_LEVEL.

### Where debug is used (examples)

- **main.c**: Startup banner (firmware version, CPU/peri clock, SPI speed, WiFi support, heap). NTP result. In Debug build, UART stdio is initialized (115200 baud) and the bus loop is always started (even when the Apple is not connected) so you can test with the machine off.
- **cmdhandler.c**: Test result, timeout, format (unit, block count, name), CMD_TFTPRUN parameters (dir, unit, hostname, filename).
- **udptask.cpp**: Connect status, BADAUTH, UDP pcb, event loop, SSID/WPA, WiFi connect attempts, link status, IP/gateway/netmask/DNS, DNS lookup result.
- **tftptask.cpp**, **tftprxtask.cpp**, **tftptxtask.cpp**: TFTP options, DNS lookup, server port, OACK, error packets, block capacity, tsize.
- **ntptask.cpp**: NTP request attempt, valid response.
- **network.cpp**: GetNetworkTime, TestWifi, exception.
- **fpu.c**: FPU ops (fac, arg, result) and errors (overflow, divide by zero, etc.) when the Pico does floating point for the Apple.
- **misc.c**: Hex dump of buffer; version string can append ` (DEBUG)` when NDEBUG is not set.
- **rtc.c**: RTC-related debug when NDEBUG is not set.

### Stdio in Debug vs Release

- **Debug** (`NDEBUG` not defined): `stdio_uart_init()` is called (default 115200 baud). `setbuf(stdout, NULL)` so output is unbuffered. Debug prints go to **UART** (e.g. GPIO UART on the Pico).
- **Release** (`NDEBUG` defined): `stdio_set_driver_enabled(&stdio_uart, false)` so UART stdio is disabled. All `DEBUG_PRINTF` etc. compile to no-ops.

So in practice, “Pico debug mode” = **Debug build** → UART on, `DEBUG_PRINTF` (and other level macros) active; **Release build** → UART off, no debug print.

### lwIP (Pico W / Pico 2 W)

In **`pico/lwipopts.h`**, when **NDEBUG** is not defined, **LWIP_DEBUG** is set to 1. Other lwIP debug options (e.g. TCP_DEBUG, UDP_DEBUG) remain off by default, but you can turn them on there for network-level debugging.

---

## Summary

| Layer        | Flag / build     | Output              | Purpose                          |
|-------------|------------------|---------------------|----------------------------------|
| **Firmware** (6502) | `DEBUG EQU TRUE` in `buildflags.inc` | Serial Port 1 (ACIA $C098–$C09B) | SmartPort/ProDOS call tracing on the Apple |
| **Pico** (C/C++)   | Debug build (no `NDEBUG`)     | UART (e.g. 115200)  | Commands, TFTP, WiFi, NTP, FPU, heap, etc. |

- **Firmware debug**: Rebuild ROM with `DEBUG EQU TRUE`; connect a terminal to the IIc’s Serial Port 1.
- **Pico debug**: Build from `pico_debug` or `pico2_debug` and connect a serial terminal to the Pico’s UART (or USB if used for stdio) to see `DEBUG_PRINTF` and related output.

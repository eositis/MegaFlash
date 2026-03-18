## Sharing `spi0` between Winbond flash and SD card

This note documents what is needed to run a standard SD card on the same `spi0` bus as the existing Winbond SPI flash chips, assuming we:

- Keep the existing pins:
  - `SCK_PIN = 2`
  - `MOSI_PIN = 3`
  - `MISO_PIN = 4`
  - `CS0_PIN = 5` (flash chip #0)
  - `CS1_PIN = 28` (flash chip #1)
- Add a **new SD‑card chip select**:
  - `CS2_PIN = 27` (SD card only)

All devices share SCK/MOSI/MISO, each has its own `/CS`.

---

## 1. Hardware requirements

- **Dedicated SD CS line**
  - Wire `GPIO27` to the SD card `/CS`.
  - Keep `CS0` and `CS1` wired to the existing Winbond flash chips.
  - Ensure only one of `CS0`, `CS1`, `CS2` is ever low at a time.

- **Voltage and power**
  - The RP2040 and SD card both operate at **3.3 V I/O**.
  - Power the SD socket from 3.3 V and provide local decoupling capacitors (e.g. 0.1 µF + a few µF near the socket).

- **Pull‑ups and optional signals**
  - Provide pull‑ups on SD `MOSI`, `MISO`, `/CS`, and any unused SD data pins if the socket exposes them.
  - Optionally wire **card‑detect** and **write‑protect** signals if needed by firmware later.

- **Signal integrity and length guidelines**
  - Keep SD traces reasonably short and routed like the existing SPI lines over a solid ground plane.
  - At the current **flash speed of 75 MHz**, treat SCK and data as high‑speed lines:
    - Aim for **≤ 5–7 cm (≈ 2–3")** from RP2040 to the farthest flash device, with minimal stubs.
    - Avoid “flying leads” or loose jumper wiring at this speed; keep everything on PCB traces.
  - For SD‑card operation at a more conservative **25 MHz**:
    - On a PCB, **10–15 cm (≈ 4–6")** trace length is typically safe if routed cleanly and length‑matched between SCK/MOSI/MISO.
    - For short cables or daughtercards, keep SPI wiring in the **5–10 cm (≈ 2–4")** range, with SCK and data routed close to ground (e.g. ribbon with interleaved GND) and minimal loop area.
    - If you must go longer than this, plan to **lower the SPI clock (e.g. 10–12 MHz)** or add small **series resistors** (≈ 22–47 Ω) near SCK (and optionally MOSI) to tame ringing.

---

## 2. SPI / CS handling in firmware

### 2.1 CS pin definitions and initialization

- Add a constant for the SD CS pin, e.g. in the same place as `CS0_PIN`/`CS1_PIN`:
  - `CS2_PIN = 27;  // SD card /CS`
- In `InitSpi()`:
  - Call `gpio_init(CS2_PIN);`.
  - Drive `CS2_PIN` high and set it as an output, consistent with `CS0_PIN`/`CS1_PIN`.
  - Apply the same slew‑rate and drive‑strength settings as other SPI outputs.
  - At the end of initialization, **all CS pins must be high** (`CS0`, `CS1`, and `CS2`).

### 2.2 Separate CS helpers for flash and SD

- Keep the existing flash helpers in `flash.c`:
  - `enable_spi0(deviceNum)` lowers **only** `CS0` or `CS1` according to `deviceNum`.
  - `disable_spi0()` raises **both** `CS0` and `CS1`.
- Add **SD‑specific CS helpers**:
  - `enable_sd()` → set `CS2` low.
  - `disable_sd()` → set `CS2` high.
- SD code must:
  - Call `enable_sd()` immediately before transfers.
  - Call `disable_sd()` immediately after.
  - Assume `CS0` and `CS1` are already high (because all flash calls end with `disable_spi0()`).

### 2.3 Shared SPI configuration: mode and speed

- **SPI mode**
  - The flash code uses **SPI mode 3** (`CPOL = 0`, `CPHA = 1`).
  - SD cards in SPI mode support **mode 0 or 3**, so we can re‑use **mode 3** for both.
- **SPI speed**
  - Flash:
    - Initialized at **25 MHz** (`SPI_SPEED_INIT`), then raised to **75 MHz** (`SPI_SPEED_FINAL`) after flash setup.
  - SD card:
    - Requires **≤400 kHz** during the initial reset/identification sequence.
    - After initialization, can run at higher speeds (e.g. 12–25 MHz) over SPI.
- Because both share `spi0`, code must either:
  - **Switch baud rates** when changing between flash and SD:
    - Before SD init: `spi_set_baudrate(spi0, 400000);`
    - After SD init: `spi_set_baudrate(spi0, SD_WORKING_BAUD);` (e.g. 12–25 MHz).
    - Before heavy flash operations: restore **`SPI_SPEED_FINAL`**.
  - Or choose a **single compromise speed** for both (e.g. 25 MHz) and accept reduced peak flash performance.

### 2.4 Mutual exclusion on the SPI bus

- Flash access already uses a **recursive mutex** (`MUTEXLOCK` / `MUTEXUNLOCK`) to protect flash operations (SPI + DMA).
- SD access must also respect bus ownership:
  - Either:
    - Re‑use the **same mutex** around SD transactions, or
  - Introduce a higher‑level **SPI bus lock** that both flash and SD callers acquire before touching `spi0` or changing its baud rate.
- The goal is to guarantee:
  - Only **one device is selected (CS low)** at a time.
  - No code changes SPI configuration while another transfer is in progress.

---

## 3. SD‑card protocol requirements (SPI mode)

Adding the SD card to the bus is not just a wiring change; we also need an **SD‑over‑SPI driver**:

- **Initialization sequence**
  - Provide at least 74 clock cycles with all CS lines high.
  - With `CS2` low, send:
    - `CMD0` (GO_IDLE_STATE) to enter SPI mode.
    - `CMD8`, `ACMD41`, `CMD58`, etc., per the SD spec, until the card leaves idle.
  - Set the block length (typically 512 bytes) and determine SDSC vs SDHC/SDXC.

- **Read/write commands**
  - Implement single and multi‑block operations:
    - Reads: `CMD17` / `CMD18`.
    - Writes: `CMD24` / `CMD25`.
  - Handle data tokens, CRC, and busy tokens on MISO.

- **Integration with existing media layer**
  - Decide whether to expose SD as:
    - Another logical “unit” alongside the ProDOS drives backed by flash, or
    - A separate storage (e.g. FAT volume) that the firmware uses to store/load disk images.

---

## 4. Compatibility summary

- With `GPIO27` dedicated as `CS2` for the SD card and SCK/MOSI/MISO shared:
  - **Electrical compatibility** is fine (all devices are 3.3 V SPI peripherals).
  - **Bus sharing** is safe provided:
    - Only one CS is low at a time.
    - All devices tri‑state MISO when their CS is high (Winbond flash and SD cards do).
- The main work is:
  - Extending the **CS handling and SPI configuration** in firmware.
  - Implementing an **SD‑over‑SPI driver** that coexists with the existing Winbond flash driver on `spi0`.


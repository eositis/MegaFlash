/*
 * Standalone SPI NOR tests for MegaFlash (same CS/SCK/MOSI/MISO as flash.c).
 *
 * 1) Read sweep (blocking fast read vs 10 MHz reference).
 * 2) XMODEM-like upload simulation: 64 KiB erase + DMA CRC + dual page program + DMA verify
 *    (same logic as WriteBlockForImageTransfer / tsWriteOneBlockWithoutErase). DESTRUCTIVE.
 *
 * Build (example; use the same arm-none-eabi toolchain as megaflash — see BUILD-REQUIREMENTS.md):
 *   cmake -B si_test_pico2_w -S . -DPICO_BOARD=pico2_w -DMEGAFLASH_BUILD_FLASH_SI_TEST=ON \
 *     -DCMAKE_C_COMPILER=/path/to/arm-none-eabi-gcc -DCMAKE_CXX_COMPILER=/path/to/arm-none-eabi-g++
 *   cmake --build si_test_pico2_w --target flash_si_test
 */

#include <stdio.h>
#include <string.h>
#include "build_id.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"

#include "flash_si_pins.h"
#include "flash_si_softspi.h"
#include "xmodem_like.h"

static void jedec_read(unsigned device, uint8_t out[3]) {
    const uint8_t tx[4] = {0x9f, 0, 0, 0};
    uint8_t rx[4];
    flash_si_cs_low(device);
    spi_write_read_blocking(spi0, tx, rx, 4);
    flash_si_cs_both_high();
    out[0] = rx[1];
    out[1] = rx[2];
    out[2] = rx[3];
}

static void cmd_enter_4byte(unsigned device) {
    const uint8_t msg[] = {0xb7};
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, 1);
    flash_si_cs_both_high();
}

static void write_disable(unsigned device) {
    const uint8_t msg[] = {0x04};
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, 1);
    flash_si_cs_both_high();
}

static void write_enable_vsr(unsigned device) {
    const uint8_t msg[] = {0x50};
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, 1);
    flash_si_cs_both_high();
}

static uint8_t read_sr3(unsigned device) {
    uint8_t tx[2] = {0x15, 0};
    uint8_t rx[2];
    flash_si_cs_low(device);
    spi_write_read_blocking(spi0, tx, rx, 2);
    flash_si_cs_both_high();
    return rx[1];
}

static void write_sr3_volatile(unsigned device, uint8_t value) {
    uint8_t msg[2] = {0x11, value};
    write_disable(device);
    write_enable_vsr(device);
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, 2);
    flash_si_cs_both_high();
}

static void fast_read_512(unsigned device, uint32_t addr, uint8_t *buf) {
    uint8_t msg[6];
    msg[0] = 0x0c;
    msg[1] = (uint8_t)(addr >> 24);
    msg[2] = (uint8_t)(addr >> 16);
    msg[3] = (uint8_t)(addr >> 8);
    msg[4] = (uint8_t)addr;
    msg[5] = 0;
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, sizeof msg);
    spi_read_blocking(spi0, 0, buf, 512);
    flash_si_cs_both_high();
}

enum { READ_LEN = 512, REF_ITERS = 8, STRESS_ITERS = 64 };

static bool capture_reference(unsigned device, uint8_t *ref) {
    spi_set_baudrate(spi0, 10 * 1000 * 1000);
    uint8_t a[READ_LEN];
    uint8_t b[READ_LEN];
    fast_read_512(device, 0, a);
    for (int i = 0; i < REF_ITERS; i++) {
        fast_read_512(device, 0, b);
        if (memcmp(a, b, READ_LEN) != 0)
            return false;
    }
    memcpy(ref, a, READ_LEN);
    return true;
}

static unsigned stress_at_baud(unsigned device, uint32_t hz, const uint8_t *ref) {
    spi_set_baudrate(spi0, hz);
    uint8_t buf[READ_LEN];
    unsigned bad = 0;
    for (unsigned i = 0; i < STRESS_ITERS; i++) {
        fast_read_512(device, 0, buf);
        if (memcmp(buf, ref, READ_LEN) != 0)
            bad++;
    }
    return bad;
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    flash_si_spi_init();

    printf("\n=== flash_si_test MegaFlash SPI NOR ===\n");
    printf("build %s  compiled %s %s (expect: soft-SPI after read sweep; xfer row0=500kHz)\n",
           FIRMWARE_BUILD_TIMESTAMP_STR, __DATE__, __TIME__);
    printf("clk_sys=%u Hz clk_peri=%u Hz\n\n",
           (unsigned)clock_get_hz(clk_sys), (unsigned)clock_get_hz(clk_peri));

    static const uint32_t baud_targets[] = {
        10 * 1000 * 1000,  12 * 1000 * 1000, 15 * 1000 * 1000, 20 * 1000 * 1000,
        25 * 1000 * 1000,  30 * 1000 * 1000, 37 * 1000 * 1000 + 500 * 1000,
        50 * 1000 * 1000,  62 * 1000 * 1000 + 500 * 1000, 75 * 1000 * 1000,
        93 * 1000 * 1000 + 750 * 1000, 100 * 1000 * 1000,
    };

    for (unsigned dev = 0; dev < 2; dev++) {
        uint8_t id[3];
        jedec_read(dev, id);
        printf("CS%u JEDEC %02X %02X %02X", dev, id[0], id[1], id[2]);
        if (id[0] == 0xff && id[1] == 0xff && id[2] == 0xff) {
            printf(" (no device?)\n\n");
            continue;
        }
        printf("\n");

        cmd_enter_4byte(dev);

        uint8_t ref[READ_LEN];
        if (!capture_reference(dev, ref)) {
            printf("CS%u FAIL: reference reads unstable at 10 MHz (check wiring / voltage)\n\n", dev);
            continue;
        }

        uint8_t sr3_saved = read_sr3(dev);
        printf("CS%u SR3(read)=%02X\n", dev, sr3_saved);

        printf("CS%u phase default_SR3: baud_target,baud_actual,failures/%u\n", dev, STRESS_ITERS);
        for (unsigned bi = 0; bi < sizeof(baud_targets) / sizeof(baud_targets[0]); bi++) {
            uint32_t want = baud_targets[bi];
            spi_set_baudrate(spi0, want);
            uint32_t got = spi_get_baudrate(spi0);
            unsigned bad = stress_at_baud(dev, want, ref);
            printf("%u,%u,%u\n", (unsigned)want, (unsigned)got, bad);
        }

        uint8_t sr3_max_drv = (uint8_t)(sr3_saved & 0x9fu);
        write_sr3_volatile(dev, sr3_max_drv);
        printf("CS%u phase SR3_DRV_cleared (target 100%%): now SR3=%02X\n", dev, read_sr3(dev));
        printf("CS%u phase SR3_DRV_cleared: baud_target,baud_actual,failures/%u\n", dev, STRESS_ITERS);
        for (unsigned bi = 0; bi < sizeof(baud_targets) / sizeof(baud_targets[0]); bi++) {
            uint32_t want = baud_targets[bi];
            unsigned bad = stress_at_baud(dev, want, ref);
            uint32_t got = spi_get_baudrate(spi0);
            printf("%u,%u,%u\n", (unsigned)want, (unsigned)got, bad);
        }

        write_sr3_volatile(dev, sr3_saved);
        printf("CS%u restored SR3=%02X\n\n", dev, read_sr3(dev));
    }

    printf("=== read sweep done ===\n");

    flash_si_softspi_run_diag();

    flash_si_run_xmodem_like_test();

    for (;;)
        tight_loop_contents();
}

#include "flash_si_softspi.h"
#include <stdio.h>
#include "flash_si_pins.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

/* Half-bit delay → order ~100 kHz SCK (well below SI analyzer need). */
#define FLASH_SI_SOFT_HALF_US 5u

#define TEST_TAIL_ADDR 0x03FF0000u

static void sb_delay(void) { busy_wait_us_32(FLASH_SI_SOFT_HALF_US); }

/* Mode 1: MOSI stable before rising edge; sample MISO while SCK high before falling. */
static uint8_t soft_xfer_byte(uint8_t tx) {
    uint8_t rx = 0;
    for (int bit = 7; bit >= 0; --bit) {
        gpio_put(FLASH_SI_MOSI_PIN, (tx >> bit) & 1u);
        sb_delay();
        gpio_put(FLASH_SI_SCK_PIN, 1);
        sb_delay();
        rx = (uint8_t)((rx << 1) | (gpio_get(FLASH_SI_MISO_PIN) ? 1u : 0u));
        gpio_put(FLASH_SI_SCK_PIN, 0);
        sb_delay();
    }
    return rx;
}

static void soft_cmd_only(unsigned device, uint8_t cmd) {
    flash_si_cs_low(device);
    (void)soft_xfer_byte(cmd);
    flash_si_cs_both_high();
    busy_wait_us_32(4);
}

static void soft_jedec(unsigned device, uint8_t id[3]) {
    flash_si_cs_low(device);
    (void)soft_xfer_byte(0x9f);
    id[0] = soft_xfer_byte(0);
    id[1] = soft_xfer_byte(0);
    id[2] = soft_xfer_byte(0);
    flash_si_cs_both_high();
}

static uint8_t soft_read_sr1(unsigned device) {
    flash_si_cs_low(device);
    (void)soft_xfer_byte(0x05);
    uint8_t v = soft_xfer_byte(0);
    flash_si_cs_both_high();
    return v;
}

static uint8_t soft_read_sr2(unsigned device) {
    flash_si_cs_low(device);
    (void)soft_xfer_byte(0x35);
    uint8_t v = soft_xfer_byte(0);
    flash_si_cs_both_high();
    return v;
}

static uint8_t soft_read_sr3(unsigned device) {
    flash_si_cs_low(device);
    (void)soft_xfer_byte(0x15);
    uint8_t v = soft_xfer_byte(0);
    flash_si_cs_both_high();
    return v;
}

static void soft_enter_4byte(unsigned device) { soft_cmd_only(device, 0xb7); }

static void soft_write_disable(unsigned device) { soft_cmd_only(device, 0x04); }

static void soft_write_enable(unsigned device) { soft_cmd_only(device, 0x06); }

static void soft_fast_read_n(unsigned device, uint32_t addr, uint8_t *out, unsigned n) {
    flash_si_cs_low(device);
    (void)soft_xfer_byte(0x0c);
    (void)soft_xfer_byte((uint8_t)(addr >> 24));
    (void)soft_xfer_byte((uint8_t)(addr >> 16));
    (void)soft_xfer_byte((uint8_t)(addr >> 8));
    (void)soft_xfer_byte((uint8_t)addr);
    (void)soft_xfer_byte(0);
    for (unsigned i = 0; i < n; i++)
        out[i] = soft_xfer_byte(0);
    flash_si_cs_both_high();
}

void flash_si_softspi_run_diag(void) {
    printf("\n=== Soft SPI GPIO (bit-bang, ~100 kHz class, CPOL0 CPHA1) ===\n");
    printf("spi_deinit(spi0); same CS/SCK/MOSI/MISO as production.\n");

    spi_deinit(spi0);
    gpio_set_function(FLASH_SI_SCK_PIN, GPIO_FUNC_SIO);
    gpio_set_function(FLASH_SI_MOSI_PIN, GPIO_FUNC_SIO);
    gpio_set_function(FLASH_SI_MISO_PIN, GPIO_FUNC_SIO);

    gpio_init(FLASH_SI_SCK_PIN);
    gpio_init(FLASH_SI_MOSI_PIN);
    gpio_init(FLASH_SI_MISO_PIN);
    gpio_set_dir(FLASH_SI_SCK_PIN, true);
    gpio_set_dir(FLASH_SI_MOSI_PIN, true);
    gpio_set_dir(FLASH_SI_MISO_PIN, false);
    gpio_put(FLASH_SI_SCK_PIN, 0);
    gpio_pull_down(FLASH_SI_MISO_PIN);

    for (unsigned dev = 0; dev < 2u; dev++) {
        uint8_t id[3];
        soft_jedec(dev, id);
        printf("CS%u soft JEDEC %02X %02X %02X\n", (unsigned)dev, id[0], id[1], id[2]);

        soft_enter_4byte(dev);
        uint8_t sr1 = soft_read_sr1(dev);
        uint8_t sr2 = soft_read_sr2(dev);
        uint8_t sr3 = soft_read_sr3(dev);
        uint8_t a0[8], a1[8];
        soft_fast_read_n(dev, 0, a0, sizeof a0);
        soft_fast_read_n(dev, TEST_TAIL_ADDR, a1, sizeof a1);
        printf("CS%u soft SR1=%02X SR2=%02X SR3=%02X fastread8@0:", (unsigned)dev, sr1, sr2, sr3);
        for (unsigned i = 0; i < sizeof a0; i++)
            printf(" %02X", a0[i]);
        printf(" @%08lX:", (unsigned long)TEST_TAIL_ADDR);
        for (unsigned i = 0; i < sizeof a1; i++)
            printf(" %02X", a1[i]);
        printf("\n");

        soft_write_enable(dev);
        busy_wait_us_32(20);
        uint8_t sr1_wel = soft_read_sr1(dev);
        soft_write_disable(dev);
        busy_wait_us_32(10);
        printf("CS%u soft after WEN: SR1=%02X (expect WEL bit if command seen)\n", (unsigned)dev, sr1_wel);
    }

    flash_si_spi_init();
    printf("=== Soft SPI done (HW SPI restored) ===\n");
}

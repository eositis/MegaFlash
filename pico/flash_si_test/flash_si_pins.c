#include "flash_si_pins.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#ifndef OC_RP2350
#define OC_RP2350 0
#endif

void flash_si_cs_both_high(void) {
#if OC_RP2350
    asm volatile("nop");
#endif
    asm volatile("nop");
    gpio_set_mask(1u << FLASH_SI_CS0_PIN | 1u << FLASH_SI_CS1_PIN);
#if OC_RP2350
    asm volatile("nop");
#endif
    asm volatile("nop");
}

void flash_si_cs_low(unsigned device) {
#if OC_RP2350
    asm volatile("nop");
#endif
    asm volatile("nop");
    gpio_clr_mask(device == 0 ? (1u << FLASH_SI_CS0_PIN) : (1u << FLASH_SI_CS1_PIN));
#if OC_RP2350
    asm volatile("nop");
#endif
    asm volatile("nop");
}

void flash_si_spi_init(void) {
    gpio_init(FLASH_SI_CS0_PIN);
    gpio_init(FLASH_SI_CS1_PIN);
    gpio_set_mask(1u << FLASH_SI_CS0_PIN | 1u << FLASH_SI_CS1_PIN);
    gpio_set_dir_out_masked(1u << FLASH_SI_CS0_PIN | 1u << FLASH_SI_CS1_PIN);

    gpio_set_slew_rate(FLASH_SI_CS0_PIN, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(FLASH_SI_CS1_PIN, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(FLASH_SI_SCK_PIN, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(FLASH_SI_MOSI_PIN, GPIO_SLEW_RATE_FAST);

    gpio_set_drive_strength(FLASH_SI_CS0_PIN, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_drive_strength(FLASH_SI_CS1_PIN, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_drive_strength(FLASH_SI_SCK_PIN, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_drive_strength(FLASH_SI_MOSI_PIN, GPIO_DRIVE_STRENGTH_8MA);

    gpio_set_pulls(FLASH_SI_CS0_PIN, false, false);
    gpio_set_pulls(FLASH_SI_CS1_PIN, false, false);
    gpio_set_pulls(FLASH_SI_SCK_PIN, false, false);
    gpio_set_pulls(FLASH_SI_MOSI_PIN, false, false);

    gpio_set_function(FLASH_SI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_SI_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_SI_MISO_PIN, GPIO_FUNC_SPI);
    gpio_pull_down(FLASH_SI_MISO_PIN);

    spi_init(spi0, (int)FLASH_SI_SPI_SPEED_INIT);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    uint8_t dummy = 0;
    spi_read_blocking(spi0, 0, &dummy, 1);
}

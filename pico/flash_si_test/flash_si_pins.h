#ifndef FLASH_SI_PINS_H
#define FLASH_SI_PINS_H

#include <stdint.h>

void flash_si_spi_init(void);
void flash_si_cs_both_high(void);
void flash_si_cs_low(unsigned device);

#define FLASH_SI_CS0_PIN 5u
#define FLASH_SI_CS1_PIN 28u
#define FLASH_SI_SCK_PIN 2u
#define FLASH_SI_MOSI_PIN 3u
#define FLASH_SI_MISO_PIN 4u

#define FLASH_SI_SPI_SPEED_INIT 25000000u
#define FLASH_SI_SPI_SPEED_FINAL 75000000u

#endif

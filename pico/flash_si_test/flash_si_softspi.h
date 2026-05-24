#ifndef FLASH_SI_SOFTSPI_H
#define FLASH_SI_SOFTSPI_H

/* Bit-bang SPI on same pins as flash_si_pins (CPOL0 CPHA1 to match spi_set_format in flash_si_spi_init).
 * Calls spi_deinit(spi0), runs slow transfers, then flash_si_spi_init() restores hardware SPI. */
void flash_si_softspi_run_diag(void);

#endif

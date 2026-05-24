/*
 * Mirrors MegaFlash XMODEM image path: WriteBlockForImageTransfer → erase 64 KiB once,
 * then tsWriteOneBlockWithoutErase (DMA CRC of source, program two pages, DMA read verify).
 * Overwrites the last 64 KiB sector of a 64 MiB part (0x03FF0000..0x03FFFFFF).
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/dma.h"
#include "hardware/spi.h"

#include "defines.h"
#include "dmamemops.h"
#include "flash_si_pins.h"
#include "xmodem_like.h"

#ifndef OC_RP2350
#define OC_RP2350 0
#endif

#define REPEATED_TX_DATA 0

/* Must match flash.c BITINVERSION for image transfer (tsWriteOneBlockAlreadyErased_Public). */
#define FLASH_SI_BITINVERSION 1

/* Last 64 KiB of 64 MiB (512 Mbit) linear map; adjust if chip size differs. */
#define TEST_SECTOR_BASE 0x03FF0000u
#define XMODEM_SIM_BLOCKS 16u

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

static uint8_t read_sr1(unsigned device) {
    uint8_t tx[2] = {0x05, 0};
    uint8_t rx[2];
    flash_si_cs_low(device);
    spi_write_read_blocking(spi0, tx, rx, 2);
    flash_si_cs_both_high();
    return rx[1];
}

/* One CS assert, 0x05, then N consecutive SR1 reads (matches wait_busy streaming). */
static void posterase_sr1_held_burst(unsigned device, unsigned n) {
    const uint8_t cmd = 0x05;
    flash_si_cs_low(device);
    spi_write_blocking(spi0, &cmd, 1);
    printf(" heldBurst%u=", n);
    for (unsigned i = 0; i < n; i++) {
        uint8_t r;
        busy_wait_us_32(2);
        spi_read_blocking(spi0, REPEATED_TX_DATA, &r, 1);
        printf("%02X", r);
    }
    flash_si_cs_both_high();
    printf("\n");
}

static uint8_t read_sr2(unsigned device) {
    uint8_t tx[2] = {0x35, 0};
    uint8_t rx[2];
    flash_si_cs_low(device);
    spi_write_read_blocking(spi0, tx, rx, 2);
    flash_si_cs_both_high();
    return rx[1];
}

static uint8_t read_sr3(unsigned device) {
    uint8_t tx[2] = {0x15, 0};
    uint8_t rx[2];
    flash_si_cs_low(device);
    spi_write_read_blocking(spi0, tx, rx, 2);
    flash_si_cs_both_high();
    return rx[1];
}

static void wait_busy(unsigned device) {
    uint8_t buf[1] = {0x05};
    flash_si_cs_low(device);
    spi_write_blocking(spi0, buf, 1);
    do {
        busy_wait_us_32(2);
        spi_read_blocking(spi0, REPEATED_TX_DATA, buf, 1);
    } while (buf[0] & 0x01u);
    flash_si_cs_both_high();
}

static void write_enable(unsigned device) {
    const uint8_t msg[] = {0x06};
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, 1);
    flash_si_cs_both_high();
}

static void erase_64k(unsigned device, uint32_t address) {
    uint8_t msg[5];
    msg[0] = 0xdc;
    msg[1] = (uint8_t)(address >> 24);
    msg[2] = (uint8_t)(address >> 16);
    msg[3] = (uint8_t)(address >> 8);
    msg[4] = (uint8_t)address;

    write_enable(device);
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, 5);
    flash_si_cs_both_high();
    sleep_ms(140);
    wait_busy(device);
}

static void program_page(unsigned device, uint32_t page_address, const uint8_t *src) {
    uint8_t msg[5];
    msg[0] = 0x12;
    msg[1] = (uint8_t)(page_address >> 24);
    msg[2] = (uint8_t)(page_address >> 16);
    msg[3] = (uint8_t)(page_address >> 8);
    msg[4] = (uint8_t)page_address;

    write_enable(device);
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, 5);
    spi_write_blocking(spi0, src, PAGESIZE);
    flash_si_cs_both_high();
    busy_wait_us_32(300);
    wait_busy(device);
}

#if FLASH_SI_BITINVERSION
static void copy_bit_inversion(uint8_t *dest, const uint8_t *src, uint32_t len) {
    assert(len % 4u == 0);
    uint32_t *d = (uint32_t *)dest;
    const uint32_t *s = (const uint32_t *)src;
    assert(((uintptr_t)d & 3u) == 0);
    assert(((uintptr_t)s & 3u) == 0);
    for (uint32_t i = len / 4u; i != 0; --i)
        *d++ = ~*s++;
}
#endif

static bool is_empty_page(const uint8_t *src) {
    const uint32_t *p = (const uint32_t *)src;
    for (unsigned i = 0; i < PAGESIZE / 4u; i++) {
        if (p[i] != 0xffffffffu)
            return false;
    }
    return true;
}

/* Same structure as flash.c ReadFromFlashByDMA (RX sniff CRC + timeout). */
static uint32_t read_from_flash_dma(uint8_t *dest, uint32_t len, bool *success_out) {
    static bool configured;
    static int rx_channel;
    static dma_channel_config_t rx_cfg;
    static uint32_t dmatimeout_us;

    if (!configured) {
        configured = true;
        rx_channel = dma_claim_unused_channel(true);
        rx_cfg = dma_channel_get_default_config(rx_channel);
        channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
        channel_config_set_dreq(&rx_cfg, spi_get_dreq(spi0, false));
        channel_config_set_write_increment(&rx_cfg, true);
        channel_config_set_read_increment(&rx_cfg, false);
        channel_config_set_sniff_enable(&rx_cfg, true);
    }
    {
        const uint32_t baud = spi_get_baudrate(spi0);
        dmatimeout_us = (baud >= 50000000ul) ? 3u : (6u * 25000000ul / baud);
        if (dmatimeout_us < 3u)
            dmatimeout_us = 3u;
    }

    dma_channel_configure(rx_channel, &rx_cfg, dest, &spi_get_hw(spi0)->dr, len, false);
    SetCRC32Seed(rx_channel, DEFAULT_CRC32_SEED);
    dma_start_channel_mask(1u << rx_channel);

    for (size_t i = 0; i < len; ++i) {
        while (!spi_is_writable(spi0))
            tight_loop_contents();
        spi_get_hw(spi0)->dr = REPEATED_TX_DATA;
    }

    *success_out = true;
    const uint32_t t0 = time_us_32();
    do {
        if (!dma_channel_is_busy(rx_channel)) {
            spi_get_hw(spi0)->icr = SPI_SSPICR_RORIC_BITS;
            return GetCRC();
        }
    } while ((time_us_32() - t0) < dmatimeout_us);

    *success_out = false;
    dma_channel_abort(rx_channel);
    while (spi_is_readable(spi0))
        (void)spi_get_hw(spi0)->dr;
    spi_get_hw(spi0)->icr = SPI_SSPICR_RORIC_BITS;
    return 0;
}

static uint32_t read_one_block_dma_crc(unsigned device, uint32_t block_address, uint8_t *dest,
                                       bool *dma_ok_out) {
    uint8_t msg[6];
    msg[0] = 0x0c;
    msg[1] = (uint8_t)(block_address >> 24);
    msg[2] = (uint8_t)(block_address >> 16);
    msg[3] = (uint8_t)(block_address >> 8);
    msg[4] = (uint8_t)block_address;
    msg[5] = 0;

    bool success;
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, sizeof msg);
    uint32_t crc = read_from_flash_dma(dest, BLOCKSIZE, &success);
    flash_si_cs_both_high();

    if (!success) {
        flash_si_cs_low(device);
        spi_write_blocking(spi0, msg, sizeof msg);
        spi_read_blocking(spi0, REPEATED_TX_DATA, dest, BLOCKSIZE);
        flash_si_cs_both_high();
        crc = CRC32Aligned(dest, BLOCKSIZE);
    }
    if (dma_ok_out)
        *dma_ok_out = success;
    return crc;
}

static void read_one_block_blocking_only(unsigned device, uint32_t block_address, uint8_t *dest) {
    uint8_t msg[6];
    msg[0] = 0x0c;
    msg[1] = (uint8_t)(block_address >> 24);
    msg[2] = (uint8_t)(block_address >> 16);
    msg[3] = (uint8_t)(block_address >> 8);
    msg[4] = (uint8_t)block_address;
    msg[5] = 0;
    flash_si_cs_low(device);
    spi_write_blocking(spi0, msg, sizeof msg);
    spi_read_blocking(spi0, REPEATED_TX_DATA, dest, BLOCKSIZE);
    flash_si_cs_both_high();
}

/* Same logic as flash.c tsWriteOneBlockWithoutErase (no mutex). */
static bool write_one_block_already_erased(unsigned device, uint32_t block_address,
                                           const uint8_t *src_512) {
    SetCRC32Seed(GetMemoryDMAChannel(), DEFAULT_CRC32_SEED);
    CopyMemoryAlignedBG((uint8_t *)src_512, src_512, BLOCKSIZE);

    if (!is_empty_page(src_512))
        program_page(device, block_address, src_512);
    if (!is_empty_page(src_512 + PAGESIZE))
        program_page(device, block_address + PAGESIZE, src_512 + PAGESIZE);

    DMAWaitFinish();
    const uint32_t crc1 = GetCRC();

    static uint8_t __attribute__((aligned(4))) readback[BLOCKSIZE];
    bool dma_ok = true;
    const uint32_t crc2 = read_one_block_dma_crc(device, block_address, readback, &dma_ok);
    (void)dma_ok;
    return crc1 == crc2;
}

void flash_si_run_xmodem_like_test(void) {
    static bool dma_ready;
    if (!dma_ready) {
        InitDMAChannel();
        dma_ready = true;
    }

    static const uint32_t xfer_spi_bauds[] = {
        500u * 1000u, 1u * 1000u * 1000u,
        10u * 1000u * 1000u,  12u * 1000u * 1000u, 15u * 1000u * 1000u, 20u * 1000u * 1000u,
        25u * 1000u * 1000u,  30u * 1000u * 1000u, 37u * 1000u * 1000u + 500u * 1000u,
        50u * 1000u * 1000u,  62u * 1000u * 1000u + 500u * 1000u, 75u * 1000u * 1000u,
        93u * 1000u * 1000u + 750u * 1000u, 100u * 1000u * 1000u,
    };

    printf("\n=== XMODEM-like upload simulation (DESTRUCTIVE) ===\n");
    printf("Erases + reprograms last 64 KiB: 0x%08lX..0x%08lX\n",
           (unsigned long)TEST_SECTOR_BASE, (unsigned long)(TEST_SECTOR_BASE + 65535u));
    printf("Path: 64k erase + tsWriteOneBlockWithoutErase (DMA CRC, 2x page program, DMA read verify)\n");
#if FLASH_SI_BITINVERSION
    printf("Data path: BITINVERSION=1 (same as flash.c image upload — inverted before program)\n");
#endif
    printf("SPI sweep: same ladder as read sweep; per row: erase + probe64 + BLOCK0; "
           "full BLOCK0..15 only when spi_want=%u (production SPI_SPEED_FINAL).\n\n",
           (unsigned)(75u * 1000u * 1000u));

    /* Baseline: HW SPI SR2/SR3 before any xfer-loop erase (after soft-SPI HW restore). */
    spi_set_baudrate(spi0, (int)(500u * 1000u));
    printf("HW SPI baseline 500kHz SR2/SR3 (no erase this line):");
    for (unsigned dev = 0; dev < 2u; dev++) {
        uint8_t id[3];
        jedec_read(dev, id);
        if (id[0] == 0xffu && id[1] == 0xffu && id[2] == 0xffu) {
            printf(" CS%u absent", dev);
            continue;
        }
        cmd_enter_4byte(dev);
        printf(" CS%u=%02X/%02X", dev, read_sr2(dev), read_sr3(dev));
    }
    printf("\n\n");

    printf("xfer,spi_want,spi_got,CS,SR1,SR2,SR3,probe_nonff,block0\n");

    static uint8_t __attribute__((aligned(4))) blockbuf[BLOCKSIZE];
#if FLASH_SI_BITINVERSION
    static uint8_t __attribute__((aligned(4))) wirebuf[BLOCKSIZE];
#endif

    for (unsigned bi = 0; bi < sizeof(xfer_spi_bauds) / sizeof(xfer_spi_bauds[0]); bi++) {
        const uint32_t want = xfer_spi_bauds[bi];
        spi_set_baudrate(spi0, (int)want);
        const uint32_t got = spi_get_baudrate(spi0);

        for (unsigned dev = 0; dev < 2; dev++) {
            uint8_t id[3];
            jedec_read(dev, id);
            if (id[0] == 0xff && id[1] == 0xff && id[2] == 0xff) {
                printf("%u,%u,%u,%u,--,--,--,na,na\n", bi, (unsigned)want, (unsigned)got, dev);
                continue;
            }

            cmd_enter_4byte(dev);
            erase_64k(dev, TEST_SECTOR_BASE);

            if (bi == 0) {
                printf("CS%u postErase SR1 tx16+2ms gaps:", dev);
                for (int k = 0; k < 16; k++) {
                    printf("%02X", read_sr1(dev));
                    sleep_ms(2);
                }
                printf("\n");
                posterase_sr1_held_burst(dev, 12);
                printf("CS%u postErase transactional SR2=%02X SR3=%02X\n", dev, read_sr2(dev),
                       read_sr3(dev));
            }

            const uint8_t s1 = read_sr1(dev);
            const uint8_t s2 = read_sr2(dev);
            const uint8_t s3 = read_sr3(dev);

            uint8_t probe[64];
            read_one_block_blocking_only(dev, TEST_SECTOR_BASE, probe);
            unsigned nff = 0;
            for (unsigned i = 0; i < sizeof probe; i++) {
                if (probe[i] != 0xffu)
                    nff++;
            }

            for (unsigned i = 0; i < BLOCKSIZE; i++)
                blockbuf[i] = (uint8_t)(0xA5u ^ (unsigned)(i + 0u * 13u));
            blockbuf[0] = 0;
            blockbuf[1] = 0;
#if FLASH_SI_BITINVERSION
            copy_bit_inversion(wirebuf, blockbuf, BLOCKSIZE);
            const bool b0 = write_one_block_already_erased(dev, TEST_SECTOR_BASE, wirebuf);
#else
            const bool b0 = write_one_block_already_erased(dev, TEST_SECTOR_BASE, blockbuf);
#endif

            printf("%u,%u,%u,%u,%02X,%02X,%02X,%u,%s\n", bi, (unsigned)want, (unsigned)got, dev, s1, s2, s3,
                   (unsigned)nff, b0 ? "OK" : "FAIL");

            /* Full 16-block stress only at production SPI rate request (75 MHz). */
            if (want == 75u * 1000u * 1000u) {
                if (!b0) {
                    printf("CS%u @75MHz BLOCK0 triage addr=0x%08lX\n", dev, (unsigned long)TEST_SECTOR_BASE);
                    const uint32_t crc_src_plain = CRC32Aligned(
#if FLASH_SI_BITINVERSION
                        wirebuf,
#else
                        blockbuf,
#endif
                        BLOCKSIZE);
                    static uint8_t __attribute__((aligned(4))) rb2[BLOCKSIZE];
                    bool dma_ok = true;
                    const uint32_t crc2_dma =
                        read_one_block_dma_crc(dev, TEST_SECTOR_BASE, rb2, &dma_ok);
                    read_one_block_blocking_only(dev, TEST_SECTOR_BASE, rb2);
                    const uint32_t crc2_blk = CRC32Aligned(rb2, BLOCKSIZE);
                    SetCRC32Seed(GetMemoryDMAChannel(), DEFAULT_CRC32_SEED);
#if FLASH_SI_BITINVERSION
                    CopyMemoryAlignedBG(wirebuf, wirebuf, BLOCKSIZE);
#else
                    CopyMemoryAlignedBG(blockbuf, blockbuf, BLOCKSIZE);
#endif
                    DMAWaitFinish();
                    const uint32_t crc1_repeat = GetCRC();
                    printf("  triage bn0: dma_rx_ok=%u crc_src_plain=%08lX crc2_dma_sniff=%08lX "
                           "crc2_blocking=%08lX crc1_dma_repeat=%08lX\n",
                           (unsigned)dma_ok, (unsigned long)crc_src_plain, (unsigned long)crc2_dma,
                           (unsigned long)crc2_blk, (unsigned long)crc1_repeat);
                }

                unsigned extrafail = 0;
                for (unsigned bn = 1u; bn < XMODEM_SIM_BLOCKS; bn++) {
                    const uint32_t addr = TEST_SECTOR_BASE + bn * (uint32_t)BLOCKSIZE;
                    for (unsigned i = 0; i < BLOCKSIZE; i++)
                        blockbuf[i] = (uint8_t)(0xA5u ^ (unsigned)(i + bn * 13u));
                    blockbuf[0] = (uint8_t)(bn & 0xffu);
                    blockbuf[1] = (uint8_t)((bn >> 8) & 0xffu);
#if FLASH_SI_BITINVERSION
                    copy_bit_inversion(wirebuf, blockbuf, BLOCKSIZE);
                    const bool ok = write_one_block_already_erased(dev, addr, wirebuf);
#else
                    const bool ok = write_one_block_already_erased(dev, addr, blockbuf);
#endif
                    if (!ok) {
                        extrafail++;
                        printf("CS%u BLOCK%u addr=0x%08lX FAIL\n", dev, bn, (unsigned long)addr);
                        if (b0 && bn == 1u) {
                            const uint32_t crc_src_plain = CRC32Aligned(
#if FLASH_SI_BITINVERSION
                                wirebuf,
#else
                                blockbuf,
#endif
                                BLOCKSIZE);
                            static uint8_t __attribute__((aligned(4))) rb2[BLOCKSIZE];
                            bool dma_ok = true;
                            const uint32_t crc2_dma = read_one_block_dma_crc(dev, addr, rb2, &dma_ok);
                            read_one_block_blocking_only(dev, addr, rb2);
                            const uint32_t crc2_blk = CRC32Aligned(rb2, BLOCKSIZE);
                            SetCRC32Seed(GetMemoryDMAChannel(), DEFAULT_CRC32_SEED);
#if FLASH_SI_BITINVERSION
                            CopyMemoryAlignedBG(wirebuf, wirebuf, BLOCKSIZE);
#else
                            CopyMemoryAlignedBG(blockbuf, blockbuf, BLOCKSIZE);
#endif
                            DMAWaitFinish();
                            const uint32_t crc1_repeat = GetCRC();
                            printf("  triage bn1: dma_rx_ok=%u crc_src_plain=%08lX crc2_dma_sniff=%08lX "
                                   "crc2_blocking=%08lX crc1_dma_repeat=%08lX\n",
                                   (unsigned)dma_ok, (unsigned long)crc_src_plain, (unsigned long)crc2_dma,
                                   (unsigned long)crc2_blk, (unsigned long)crc1_repeat);
                        }
                    }
                }
                printf("CS%u @75MHz summary: block0=%s failures_blocks_1..15=%u\n", dev,
                       b0 ? "OK" : "FAIL", (unsigned)extrafail);
            }
        }
    }

    printf("\n=== XMODEM-like test finished ===\n");
}

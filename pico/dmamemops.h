#ifndef _DMAMEMOPS_H
#define _DMAMEMOPS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "hardware/dma.h"

//
// Default CRC Seeds
//
#define DEFAULT_CRC16_SEED 0						/* As defined in XMODEM-CRC */
#define DEFAULT_CRC32_SEED 0xffffffff

//
// Memory Operation Routines
//
void InitDMAChannel();
int GetMemoryDMAChannel();
void CopyMemory(uint8_t* dest,const uint8_t *src,const uint32_t len);
void CopyMemoryAligned(uint8_t* dest,const uint8_t *src,const uint32_t len);
void CopyMemoryAlignedBG(uint8_t* dest,const uint8_t *src,const uint32_t len);
void ZeroMemory(uint8_t *dest,const uint32_t len);
void ZeroMemoryAligned(uint8_t *dest,const uint32_t len);
void ZeroMemoryAlignedBG(uint8_t *dest,const uint32_t len);

////////////////////////////////////////////////////////////////
//wait until DMA transfer is complete
static inline void DMAWaitFinish() {
	dma_channel_wait_for_finish_blocking(GetMemoryDMAChannel());	
}

static inline void SetCRC16Seed(const uint channel, const uint32_t seed){
	dma_sniffer_set_output_invert_enabled (false);
	dma_sniffer_set_output_reverse_enabled(false);														
	dma_sniffer_enable(channel, DMA_SNIFF_CTRL_CALC_VALUE_CRC16 , true);		
	dma_sniffer_set_data_accumulator(seed);
}

static inline void SetCRC32Seed(const uint channel, const uint32_t seed) {
	dma_sniffer_set_output_invert_enabled (true);
	dma_sniffer_set_output_reverse_enabled(true);														
	dma_sniffer_enable(channel, DMA_SNIFF_CTRL_CALC_VALUE_CRC32R , true);	
	dma_sniffer_set_data_accumulator(seed);		
}

static inline void SetChecksumSeed(const uint channel, const uint32_t seed) {
  dma_sniffer_set_output_invert_enabled (false);
	dma_sniffer_set_output_reverse_enabled(false);														
	dma_sniffer_enable(channel, DMA_SNIFF_CTRL_CALC_VALUE_SUM , true);		
	dma_sniffer_set_data_accumulator(seed);
}


static inline uint32_t GetCRC() {
	return dma_sniffer_get_data_accumulator();
}

static inline uint32_t GetChecksum() {
	return dma_sniffer_get_data_accumulator();
}

uint32_t CRC16(const uint8_t *src,const uint32_t len);
uint32_t CRC16Aligned(const uint8_t *src,const uint32_t len);
uint32_t CRC32(const uint8_t *src,const uint32_t len);
uint32_t CRC32Aligned(const uint8_t *src,const uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
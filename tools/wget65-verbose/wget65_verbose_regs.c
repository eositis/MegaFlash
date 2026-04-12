/*
 * On-screen W5100 register snapshots for debugging MegaFlash Uthernet II emulation.
 * Uses the same indirect addressing path as ip65 / apps/w5100.c (slot << 4 | $C084).
 */
#include "wget65_verbose_regs.h"
#include <stdarg.h>
#include <stdio.h>

void wget65_verbose_banner(void)
{
  printf("\n");
  printf("========== WGET65V (verbose) ==========\n");
  printf("Uthernet / W5100 handshake trace\n");
  printf("=======================================\n\n");
}

void wget65_verbose_step(const char *msg)
{
  printf("[WGET65V] %s\n", msg);
}

void wget65_verbose_stepf(const char *fmt, ...)
{
  va_list ap;
  printf("[WGET65V] ");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

void wget65_dump_w5100_regs(uint8_t slot, const char *label)
{
  volatile uint8_t *mode;
  volatile uint8_t *ahi;
  volatile uint8_t *alo;
  volatile uint8_t *data;
  uint8_t mr;
  uint8_t rtr0, rtr1;
  uint8_t rmsr;
  uint8_t ptimer;
  uint8_t xor_chain;

  printf("--- W5100 dump: %s ---\n", label);
  printf("Slot (eth_init) = %u  mode reg addr = $%04X\n",
         (unsigned)slot, (unsigned)(slot << 4 | 0xC084));

  mode = (volatile uint8_t *)(slot << 4 | 0xC084);
  ahi  = mode + 1;
  alo  = mode + 2;
  data = mode + 3;

  mr = *mode;
  printf("MR read ($C0x4)     = $%02X (expect IND+AI=$03 after ip65 init)\n", mr);

  /* ip65 RTR fingerprint: lda #$07^$D0  eor data  eor data  beq */
  *ahi = 0x00;
  *alo = 0x17;
  rtr0 = *data;
  rtr1 = *data;
  xor_chain = (uint8_t)(0xD7u ^ rtr0 ^ rtr1);
  printf("RTR at $0017/18    = $%02X / $%02X  XOR chain=$%02X (want $00)\n",
         rtr0, rtr1, xor_chain);

  *ahi = 0x00;
  *alo = 0x1A;
  rmsr = *data;
  printf("RMSR ($001A)       = $%02X\n", rmsr);

  *ahi = 0x00;
  *alo = 0x28;
  ptimer = *data;
  printf("PTIMER ($0028)     = $%02X (DNS offload hint: $00=virtual)\n", ptimer);

  printf("--- end %s ---\n\n", label);
}

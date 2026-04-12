/* MegaFlash fork: screen diagnostics for Uthernet II / W5100 bring-up (not in upstream wget65). */
#ifndef WGET65_VERBOSE_REGS_H
#define WGET65_VERBOSE_REGS_H

#include <stdint.h>

void wget65_verbose_banner(void);
void wget65_verbose_step(const char *msg);
void wget65_verbose_stepf(const char *fmt, ...);
void wget65_dump_w5100_regs(uint8_t slot, const char *label);

#endif

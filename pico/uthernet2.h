#ifndef _UTHERNET2_H
#define _UTHERNET2_H

#include <stdint.h>
#include <stdbool.h>

/** Call once at startup before the bus loop. */
void U2_Init(void);

/** Advance network and drain RX; call periodically from the bus loop. */
void U2_Poll(void);

/**
 * Handle one Apple II bus access for Uthernet II at C0x4–C0x7 (slot 4: $C0C4–$C0C7).
 * busdata: lower nibble = C0x address (4–7 → W5100 ports 0–3 via &3), bit4 = read flag, bits 5–12 = write data.
 * read_byte_out: on read, set to the byte to drive on the bus; ignored on write.
 */
void U2_HandleBusAccess(uint32_t busdata, uint8_t *read_byte_out);

#endif /* _UTHERNET2_H */

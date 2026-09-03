#ifndef _UTHERNET2_H
#define _UTHERNET2_H

#include <stdint.h>
#include <stdbool.h>

/* U2 debug logging: independent of NDEBUG. Enable with -DUTHERNET2_DEBUG=1 (e.g. in Debug build).
 * Debug also enables U2_ACTIVITY_MONITOR: UART lines prefixed [u2m] (see u2_monitor.c). */
#ifndef UTHERNET2_DEBUG
#define UTHERNET2_DEBUG 0
#endif
#if UTHERNET2_DEBUG
#include <stdio.h>
#define U2_DEBUGF(...) printf("[u2] " __VA_ARGS__)
#else
#define U2_DEBUGF(...) do {} while (0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Call once at startup before the bus loop. */
void U2_Init(void);

/**
 * Request a core-0 lwIP poll from the bus path. Must stay a single SRAM store: the a2bus PIO
 * serves the 6502's next $C0C7 read ~90 ns after nDEVSEL falls, so anything slower here corrupts
 * the byte stream (§1cx). Core 0 clears the flag in U2_Net_Poll.
 */
void U2_RequestCore0NetPoll(void);

/** Set by U2_RequestCore0NetPoll on core 1; consumed by U2_Net_Poll on core 0. */
extern volatile bool u2_core0_net_wake_pending;

/**
 * Handle one Apple II bus access for Uthernet II at C0x4–C0x7 (slot 4: $C0C4–$C0C7).
 * busdata: lower nibble = C0x address (4–7 → W5100 ports 0–3 via &3), bit4 = read flag, bits 5–12 = write data.
 * read_byte_out: on read, set to the byte to drive on the bus; ignored on write.
 */
void U2_HandleBusAccess(uint32_t busdata, uint8_t *read_byte_out);

/** Byte the W5100 would return on the next read of the DATA port ($C0C7) at current ptr/MR (no increment). */
uint8_t U2_PeekDataPort(void);

/** Copy SHAR (0x0009–0x000E) from `mac` — used so ip65 DHCP/MACRAW uses the same SA as CYW43 STA. */
void U2_SetStationMacFromBytes(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* _UTHERNET2_H */

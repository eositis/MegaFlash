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

/** Advance network (lwIP); call periodically from the bus loop. Monitor UART flush is core 0 only — see U2_MonPollFlush in main. */
void U2_Poll(void);

/** Wake core 0 for lwIP poll (core 1 only; rate-limited). Phase 1 §10k. */
void U2_RequestCore0NetPoll(void);

/** Retry sock0 MACRAW SEND when Sn_CR still set after deferred TX (core 0 only). §10z P0-2. */
void U2_TryCompletePendingSocket0Send(void);

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

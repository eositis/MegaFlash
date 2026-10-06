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

/** Times the peeked memory byte differed from the later data-port read, plus the cover and audio bytes that read returned. */
void U2_RxDebugQueue(int *waits, int *feeds, int *level);

/** Last RX address the 6502 wrote after the audio byte was stored, the stored audio address, and whether that address was read. */
void U2_RxDebugAim(int *aim, int *at, int *seen);

/** Last slot cycles after the audio byte was stored. slot 0 is the newest.
 *  packed = (C0Cx nibble << 16) | (1 if read << 8) | byte. pk is the data-port prefetch, or 0. */
void U2_RxDebugCyc(int slot, int *adr, int *packed, int *pk);

#if U2_RX_AUDIT
/* §1di instrumentation counters. Core 0 writes the service-gap ones; U2_RxAuditReport (core 0)
 * emits them as NDJSON. */
extern volatile uint32_t g_u2_core0_gap_max_us, g_u2_core0_gap_5ms, g_u2_core0_gap_20ms,
                         g_u2_core0_gap_100ms, g_u2_core0_polls;

/**
 * §1dh RX delivery audit. Print the running comparison between the bytes the host committed
 * (Sn_RX_RD advance at each RECV) and the $C0C7 DATA reads we actually served. A non-zero
 * SHORT count proves the a2buslistener FIFO is discarding bus cycles (H1); lost gives the byte
 * total and the def* histogram how many cycles went missing per frame. Core 0 only.
 */
void U2_RxAuditReport(void);
#endif

/** Copy SHAR (0x0009–0x000E) from `mac` — used so ip65 DHCP/MACRAW uses the same SA as CYW43 STA. */
void U2_SetStationMacFromBytes(const uint8_t mac[6]);

/** OR Sn_IR bits (and common IR S0–S3 if IMR allows). Safe from core 0 lwIP callbacks. */
void U2_SocketIrq(int socket_i, uint8_t bits);

/**
 * Run deferred TCP/UDP/IPRAW socket commands queued by the bus core.
 * Core 0 only. Sn_CR stays set until the command is accepted.
 */
void U2_ProcessDeferredSocketCmds(void);

/**
 * Copy STA IPv4 address, gateway, and netmask into SIPR/GAR/SUBR when the
 * Apple has not written those registers since the last W5100 reset.
 */
void U2_MirrorStaNet(const uint8_t ip[4], const uint8_t gw[4], const uint8_t mask[4]);

/** Sparse UART NDJSON for socket-mode debug. force=1 always prints (capped). */
void U2_AgentLog(const char *hid, const char *msg, int a, int b, int c, int force);

/** Core 1: a write to $C0C8. Does not touch the PIO register chunks. */
void U2_NoteStage(uint8_t id);

/** Core 0: print queued $C0C8 stage bytes. */
void U2_StagePoll(void);

/** Core 1: a write to $C0C8. Does not touch the PIO register chunks. */
void U2_NoteStage(uint8_t id);

/** Core 0: print queued $C0C8 stage bytes. */
void U2_StagePoll(void);

/** Socket RX occupancy for the cover-art probe. size, live Sn_RX_RSR, shadow used. */
void U2_RxDebugStat(int socket_i, int *size, int *live_rsr, int *shadow_used);

/** Sn_CR, completed U2 bus cycles, and live Sn_RX_RD. Core 0 reads; core 1 counts cycles. */
void U2_RxDebugProbe(int socket_i, int *cr, int *bus_n, int *rx_rd);

#ifdef __cplusplus
}
#endif

#endif /* _UTHERNET2_H */

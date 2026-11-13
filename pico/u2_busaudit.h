/**
 * Uthernet II bus-pipeline audit (docs section 1ev, plan "Measure bus pipeline, fix structure").
 *
 * MEASUREMENT ONLY. Build with -DU2_BUS_AUDIT=1 (pico/build-audit.sh) into a separate image;
 * with the flag at 0 (the default, and every normal Release build) this header expands to
 * nothing and no code is generated.
 *
 * Question it answers: when the 6502 sees a wrong byte or a command lands late, is the
 * cause (a) the bus core publishing its PIO word after the next cycle already arrived,
 * (b) a cycle dropped by the 4-deep listener FIFO, (c) core 0 being away (Wi-Fi connect, NTP,
 * legacy op) so reset/commands are not serviced, or (d) none of those (emulation logic)?
 *
 * Rules the instrumentation itself obeys:
 *   - Core 1 (bus) only does counter increments, one 16-byte ring store and 3 timer reads per
 *     cycle. No printf, no flash calls, no locks.
 *   - Core 0 formats text and drains it to UART0 (GPIO 0/1) from a RAM ring without ever
 *     blocking on the UART FIFO. USB cannot carry this: a USB host gates the bus loop off.
 */
#ifndef U2_BUSAUDIT_H
#define U2_BUSAUDIT_H

#include <stdint.h>

#ifndef U2_BUS_AUDIT
#define U2_BUS_AUDIT 0
#endif

#if U2_BUS_AUDIT

#include "hardware/pio.h"
#include "hardware/timer.h"
#include "a2bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bus-cycle ring: one entry per $C0C4-$C0C7 cycle, frozen on a trigger. */
#define U2_BA_RING_N 512u
#define U2_BA_PRE 80u  /* entries printed before the trigger cycle */
#define U2_BA_POST 16u /* entries recorded after it */

typedef struct {
  uint32_t t;      /* timerawl (us) at cycle start */
  uint16_t adr;    /* u2_data_address after the cycle */
  uint8_t mode;    /* u2_mode_register after the cycle */
  uint8_t bus;     /* busdata & 0x1f: bit4 read, [3:0] address nibble */
  uint8_t wdata;   /* byte the 6502 wrote (writes) */
  uint8_t served;  /* byte the emulation returned (reads) */
  uint8_t peek;    /* data-port prefetch byte published for the NEXT cycle */
  uint8_t gotby;   /* byte the 6502 actually received (previous published word) */
  uint8_t h;       /* handler time, us (saturated) */
  uint8_t tot;     /* cycle start to word published, us (saturated) */
  uint8_t q;       /* [3:0] listener FIFO level at start, [7:4] at publish */
  uint8_t adrb_hi; /* u2_data_address high byte before the cycle */
} u2_ba_ev_t;

extern volatile uint32_t g_ba_cyc;
extern volatile uint32_t g_ba_lvl_b[5], g_ba_lvl_e[5];
extern volatile uint32_t g_ba_h_hist[8][7], g_ba_t_hist[8][7];
extern volatile uint32_t g_ba_h_max[8], g_ba_t_max[8];
extern volatile uint32_t g_ba_cmdw_h[7], g_ba_cmdw_max;
extern volatile uint32_t g_ba_pkmis[4]; /* read byte served != byte the word carried, by loc */
extern volatile uint32_t g_ba_missed[8];
extern volatile uint32_t g_ba_clamp, g_ba_resets, g_ba_reset_t;
extern volatile uint32_t g_ba_prev_chunk;
extern u2_ba_ev_t g_ba_ring[U2_BA_RING_N];
extern volatile uint32_t g_ba_head, g_ba_freeze_left, g_ba_frozen, g_ba_trig_head, g_ba_trig_code;
extern volatile uint32_t g_ba_trig_n[8], g_ba_trig_suppressed;

/* Trigger codes. */
#define U2_BA_T_CR3 1     /* a data read returned $03 from an Sn_CR address */
#define U2_BA_T_MISSED 2  /* u2_missed_command re-targeted a write */
#define U2_BA_T_CLAMP 3   /* read_socket_register found a multi-bit CR */
#define U2_BA_T_PK3 4     /* data-port read: served byte != byte carried by the published word */
#define U2_BA_T_PKOTHER 5 /* same, loc 0..2 */

/* Core-safe lifecycle trace (section 1ev, second audit). Any core pushes; core 0 prints. The tag is a
 * string literal pointer that is only dereferenced on core 0. */
#define U2_BA_TR_N 512u
typedef struct {
  volatile uint32_t seq; /* index + 1 once the entry is complete */
  uint32_t t;
  const char *tag;
  int32_t a, b, c;
} u2_ba_tr_t;
extern u2_ba_tr_t g_ba_tr[U2_BA_TR_N];
extern volatile uint32_t g_ba_tr_head;

static inline __attribute__((always_inline)) void U2_BaTrace(const char *tag, int32_t a, int32_t b, int32_t c) {
  uint32_t idx = __atomic_fetch_add(&g_ba_tr_head, 1u, __ATOMIC_RELAXED);
  u2_ba_tr_t *e = &g_ba_tr[idx & (U2_BA_TR_N - 1u)];
  __atomic_store_n(&e->seq, 0u, __ATOMIC_RELAXED);
  e->t = timer_hw->timerawl;
  e->tag = tag;
  e->a = a;
  e->b = b;
  e->c = c;
  __atomic_store_n(&e->seq, idx + 1u, __ATOMIC_RELEASE);
}

/* Core-0 breadcrumb (section 1ex). The hang found in the b148 capture cut the audit UART
 * stream mid-line while the Apple kept running. A hardware watchdog (fed only by
 * U2_BaCore0Poll) resets the chip if core 0 stops polling, and the WATCHDOG SCRATCH registers
 * survive that reset, so the next boot prints where core 0 was:
 *   scratch[0] magic, [1] last stage id, [2] time_us_32 of the last poll, [3] HardFault PC (0 = none).
 * Stage id = file<<28 | detail<<8 | phase:
 *   file 1 uthernet2_net.cpp, 2 uthernet2.c, 3 network_pump.cpp, 4 ServicePoll section, 5 udptask.cpp.
 *   lwIP lock call sites: detail = source line, phase 1 = waiting for the lock, 2 = holding it,
 *   3 = released.  file 2: detail = (op<<4)|socket, phase 0 = command started, 0xF = returned. */
#define U2_BA_WDOG_MAGIC 0xBA5E0001u
#ifndef U2_BA_WDOG_MS
#define U2_BA_WDOG_MS 6000u
#endif
#include "hardware/structs/watchdog.h"
static inline __attribute__((always_inline)) void U2_BaStage(uint32_t id) { watchdog_hw->scratch[1] = id; }

void U2_BaInit(void);       /* core 0, from U2_Init */
void U2_BaCore0Poll(void);  /* core 0: gap/stall sampling, report, ring dump, UART drain */
void U2_BaDrain(void);      /* core 0: UART drain only (does NOT count as a U2 service point) */
void U2_BaEvent(const char *tag, long a, long b); /* core 0 only */
void U2_BaNoteResetServed(int rc);                /* core 0 */

typedef struct {
  uint32_t t0;
  uint32_t th;
  uint32_t lvl;
  uint16_t adr;
} u2_ba_cyc_t;

static inline __attribute__((always_inline)) uint32_t u2_ba_bucket(uint32_t us) {
  if (us < 2u)
    return 0;
  uint32_t b = 31u - (uint32_t)__builtin_clz(us);
  return b > 6u ? 6u : b;
}

static inline __attribute__((always_inline)) void U2_BaTrigger(uint8_t code) {
  if (g_ba_frozen || g_ba_freeze_left) {
    g_ba_trig_suppressed++;
    return;
  }
  if (g_ba_trig_n[code & 7u] >= 6u) {
    g_ba_trig_suppressed++;
    return;
  }
  g_ba_trig_n[code & 7u]++;
  g_ba_trig_code = code;
  g_ba_trig_head = g_ba_head;
  g_ba_freeze_left = U2_BA_POST;
}

static inline __attribute__((always_inline)) void U2_BaMissed(uint8_t why) { g_ba_missed[why & 7u]++; }
static inline __attribute__((always_inline)) void U2_BaClamp(void) {
  g_ba_clamp++;
  U2_BaTrigger(U2_BA_T_CLAMP);
}
static inline __attribute__((always_inline)) void U2_BaResetNote(void) {
  g_ba_reset_t = timer_hw->timerawl;
  g_ba_resets++;
}

static inline __attribute__((always_inline)) void U2_BaBegin(u2_ba_cyc_t *c, uint16_t adr) {
  c->t0 = timer_hw->timerawl;
  uint32_t lvl = pio_sm_get_rx_fifo_level(pio0, SM_LISTENER);
  c->lvl = lvl > 4u ? 4u : lvl;
  c->adr = adr;
}

static inline __attribute__((always_inline)) void U2_BaHandlerDone(u2_ba_cyc_t *c) { c->th = timer_hw->timerawl - c->t0; }

/* After the PIO word has been stored. `chunk` is the word just published. */
static inline __attribute__((always_inline)) void U2_BaEnd(const u2_ba_cyc_t *c, uint32_t busdata, uint8_t read_byte, uint32_t chunk,
                            uint16_t adr_after, uint8_t mode_after) {
  uint32_t tot = timer_hw->timerawl - c->t0;
  uint32_t lvl_e = pio_sm_get_rx_fifo_level(pio0, SM_LISTENER);
  if (lvl_e > 4u)
    lvl_e = 4u;
  uint32_t loc = busdata & 3u;
  uint32_t is_read = (busdata >> 4) & 1u;
  uint32_t cls = (is_read << 2) | loc;
  uint8_t gotby = (uint8_t)(g_ba_prev_chunk >> (8u * loc));

  g_ba_cyc++;
  g_ba_lvl_b[c->lvl]++;
  g_ba_lvl_e[lvl_e]++;
  g_ba_h_hist[cls][u2_ba_bucket(c->th)]++;
  g_ba_t_hist[cls][u2_ba_bucket(tot)]++;
  if (c->th > g_ba_h_max[cls])
    g_ba_h_max[cls] = c->th;
  if (tot > g_ba_t_max[cls])
    g_ba_t_max[cls] = tot;
  /* A Sn_CR write is the command; it is a data-port write at page 4..7, offset 1. */
  if (!is_read && loc == 3u && (c->adr >> 8) >= 4u && (c->adr >> 8) <= 7u && (c->adr & 0xFFu) == 1u) {
    g_ba_cmdw_h[u2_ba_bucket(tot)]++;
    if (tot > g_ba_cmdw_max)
      g_ba_cmdw_max = tot;
  }

  if (is_read) {
    if (read_byte != gotby) {
      g_ba_pkmis[loc]++;
      /* A data-port read that differs from the prefetched byte is core 0 changing Sn_CR/Sn_SR/RSR
       * between the address write and the read (seen in the b145 audit). Count only. */
      if (loc != 3u)
        U2_BaTrigger(U2_BA_T_PKOTHER);
    }
    if (loc == 3u && gotby == 0x03u && (c->adr >> 8) >= 4u && (c->adr >> 8) <= 7u &&
        (c->adr & 0xFFu) == 1u)
      U2_BaTrigger(U2_BA_T_CR3);
  }

  if (!g_ba_frozen) {
    u2_ba_ev_t *e = &g_ba_ring[g_ba_head & (U2_BA_RING_N - 1u)];
    e->t = c->t0;
    e->adr = adr_after;
    e->mode = mode_after;
    e->bus = (uint8_t)(busdata & 0x1Fu);
    e->wdata = (uint8_t)((busdata >> 5) & 0xFFu);
    e->served = read_byte;
    e->peek = (uint8_t)(chunk >> 24);
    e->gotby = gotby;
    e->h = c->th > 255u ? 255u : (uint8_t)c->th;
    e->tot = tot > 255u ? 255u : (uint8_t)tot;
    e->q = (uint8_t)(c->lvl | (lvl_e << 4));
    e->adrb_hi = (uint8_t)(c->adr >> 8);
    g_ba_head++;
    if (g_ba_freeze_left && --g_ba_freeze_left == 0)
      g_ba_frozen = 1;
  }
  g_ba_prev_chunk = chunk;
}

#ifdef __cplusplus
}
#endif

#else /* !U2_BUS_AUDIT */

#define U2_BaStage(id) ((void)0)
#define U2_BaTrace(tag, a, b, c) ((void)0)
#define U2_BaMissed(why) ((void)0)
#define U2_BaClamp() ((void)0)
#define U2_BaResetNote() ((void)0)
#define U2_BaTrigger(code) ((void)0)
#define U2_BaInit() ((void)0)
#define U2_BaCore0Poll() ((void)0)
#define U2_BaDrain() ((void)0)
#define U2_BaEvent(tag, a, b) ((void)0)
#define U2_BaNoteResetServed(rc) ((void)0)

#endif /* U2_BUS_AUDIT */

#endif /* U2_BUSAUDIT_H */

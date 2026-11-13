/* Uthernet II bus-pipeline audit. See u2_busaudit.h. Compiled to nothing unless U2_BUS_AUDIT=1. */
#include "u2_busaudit.h"

#if U2_BUS_AUDIT

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/watchdog.h"
#include "build_id.h"

#ifndef U2_BUS_AUDIT_BAUD
#define U2_BUS_AUDIT_BAUD 115200
#endif

#define BA_TXN 32768u /* text ring, power of two */

volatile uint32_t g_ba_cyc;
volatile uint32_t g_ba_lvl_b[5], g_ba_lvl_e[5];
volatile uint32_t g_ba_h_hist[8][7], g_ba_t_hist[8][7];
volatile uint32_t g_ba_h_max[8], g_ba_t_max[8];
volatile uint32_t g_ba_cmdw_h[7], g_ba_cmdw_max;
volatile uint32_t g_ba_pkmis[4];
volatile uint32_t g_ba_missed[8];
volatile uint32_t g_ba_clamp, g_ba_resets, g_ba_reset_t;
volatile uint32_t g_ba_prev_chunk;
u2_ba_ev_t g_ba_ring[U2_BA_RING_N];
volatile uint32_t g_ba_head, g_ba_freeze_left, g_ba_frozen, g_ba_trig_head, g_ba_trig_code;
volatile uint32_t g_ba_trig_n[8], g_ba_trig_suppressed;
u2_ba_tr_t g_ba_tr[U2_BA_TR_N];
volatile uint32_t g_ba_tr_head;
static uint32_t ba_tr_tail, ba_tr_lost;

/* Core-0 side state (single thread). */
static uint8_t ba_tx[BA_TXN];
static uint32_t ba_tx_w, ba_tx_r, ba_tx_drop;
static bool ba_up, ba_wdog_on;
static uint32_t ba_last_service_us, ba_last_service_cyc;
static bool ba_have_last;
static uint32_t ba_polls, ba_gap_max, ba_gap5, ba_gap20, ba_gap100, ba_gap1000;
static uint32_t ba_gap_events;
static uint32_t ba_rxstall, ba_rxstall_events;
static uint32_t ba_rst_ok, ba_rst_skip, ba_rst_lat_max;
static uint32_t ba_next_report_us;
static uint32_t ba_dumps;

static void ba_out(const char *s, unsigned n) {
  if (!ba_up)
    return;
  if (BA_TXN - (ba_tx_w - ba_tx_r) < n) {
    ba_tx_drop++;
    return;
  }
  for (unsigned i = 0; i < n; i++)
    ba_tx[(ba_tx_w + i) & (BA_TXN - 1u)] = (uint8_t)s[i];
  ba_tx_w += n;
}

static void ba_printf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((unsigned)n >= sizeof(buf))
    n = (int)sizeof(buf) - 1;
  ba_out(buf, (unsigned)n);
}

/* Never waits: stops at the first full-FIFO check. */
static void ba_trace_drain_fatal(void);
static void ba_drain(void) {
  while (ba_tx_r != ba_tx_w && uart_is_writable(uart0)) {
    uart_get_hw(uart0)->dr = ba_tx[ba_tx_r & (BA_TXN - 1u)];
    ba_tx_r++;
  }
}

void U2_BaDrain(void) {
  if (ba_up)
    ba_drain();
}

/* ---- Fatal-path reporting (section 1ex, second audit) ----
 * b151 capture: core 0 died inside U2_Net_ListenTcp and the stacked PC was _exit(), i.e. something
 * called panic()/abort(). Nothing printed because stdio is not on this UART. These handlers write
 * to UART0 *blocking* (the chip is doomed anyway; the watchdog resets it ~6 s later), first flushing
 * the queued trace lines that core 0 never got to print, then the reason, then a stack scan. */
static void ba_fatal_flush_ring(void) {
  while (ba_tx_r != ba_tx_w) {
    while (!uart_is_writable(uart0)) {
    }
    uart_get_hw(uart0)->dr = ba_tx[ba_tx_r & (BA_TXN - 1u)];
    ba_tx_r++;
  }
}
static void ba_fatal_hex(const char *label, uint32_t v) {
  char b[40];
  int n = snprintf(b, sizeof b, " %s=0x%08lx", label, (unsigned long)v);
  ba_out(b, (unsigned)n);
}
static void __attribute__((noreturn)) ba_fatal_end(void) {
  ba_fatal_flush_ring();
  for (;;) {
  }
}
void ba_fatal_report(const char *what, const char *msg, const char *file, int line, uint32_t pc, uint32_t lr,
                     const uint32_t *sp) {
  watchdog_hw->scratch[3] = pc | 1u;
  if (ba_up) {
    ba_trace_drain_fatal();
    ba_printf("\n[ba] !!!! FATAL on core %lu: %s", (unsigned long)(*(volatile uint32_t *)0xd0000000u), what);
    if (msg)
      ba_printf(" \"%s\"", msg);
    if (file)
      ba_printf(" at %s:%d", file, line);
    ba_fatal_hex("pc", pc);
    ba_fatal_hex("lr", lr);
    ba_fatal_hex("sp", (uint32_t)(uintptr_t)sp);
    ba_printf("\n");
    if (sp) {
      ba_printf("[ba] !!!! flash return addresses on the stack (addr2line each):");
      unsigned shown = 0;
      for (unsigned i = 0; i < 160u && shown < 40u; i++) {
        uint32_t w = sp[i];
        if (w >= 0x10000000u && w < 0x10400000u && (w & 1u)) {
          ba_printf(" %08lx", (unsigned long)w);
          shown++;
        }
      }
      ba_printf("\n");
    }
  }
  ba_fatal_end();
}

/* HardFault on either core (a BKPT with no debugger lands here too). */
void ba_hardfault_c(uint32_t *sp) { ba_fatal_report("HardFault", NULL, NULL, 0, sp[6], sp[5], sp); }
void __attribute__((naked)) isr_hardfault(void) {
  __asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "b ba_hardfault_c\n");
}

/* lwIP LWIP_ASSERT (lwipopts.h routes LWIP_PLATFORM_ASSERT here in the audit image). */
void __attribute__((noreturn)) u2_ba_lwip_assert(const char *msg, const char *file, int line) {
  ba_fatal_report("lwIP assert", msg, file, line, (uint32_t)(uintptr_t)__builtin_return_address(0), 0, NULL);
  for (;;) {
  }
}

/* abort() and panic() are redirected with -Wl,--wrap (CMakeLists.txt, audit only). */
void __attribute__((noreturn)) __wrap_abort(void) {
  uint32_t sp_here;
  ba_fatal_report("abort()", NULL, NULL, 0, (uint32_t)(uintptr_t)__builtin_return_address(0), 0, &sp_here);
  for (;;) {
  }
}
void __attribute__((noreturn)) __wrap_panic(const char *fmt, ...) {
  uint32_t sp_here;
  ba_fatal_report("panic()", fmt, NULL, 0, (uint32_t)(uintptr_t)__builtin_return_address(0), 0, &sp_here);
  for (;;) {
  }
}

void U2_BaInit(void) {
  bool prev_hang = watchdog_caused_reboot() && watchdog_hw->scratch[0] == U2_BA_WDOG_MAGIC;
  uint32_t sc1 = watchdog_hw->scratch[1], sc2 = watchdog_hw->scratch[2], sc3 = watchdog_hw->scratch[3];
  watchdog_hw->scratch[0] = 0;
  watchdog_hw->scratch[3] = 0;
  uart_init(uart0, U2_BUS_AUDIT_BAUD);
  gpio_set_function(0, GPIO_FUNC_UART);
  gpio_set_function(1, GPIO_FUNC_UART);
  uart_set_fifo_enabled(uart0, true);
  ba_up = true;
  ba_next_report_us = time_us_32() + 10u * 1000u * 1000u;
  ba_printf("\n[ba] ==== U2_BUS_AUDIT image %s ====\n", FIRMWARE_BUILD_TIMESTAMP_STR);
  ba_printf("[ba] t=us since boot; cls W0..W3/R0..R3 = $C0C4+loc; H=handler us, T=start..word published us\n");
  ba_printf("[ba] buckets: <2,2-3,4-7,8-15,16-31,32-63,64+ us\n");
  if (prev_hang) {
    ba_printf("[ba] !!!! PREVIOUS BOOT WAS RESET BY THE WATCHDOG: core 0 stopped polling for %u ms\n",
              (unsigned)U2_BA_WDOG_MS);
    ba_printf("[ba] !!!! last stage=0x%08lx file=%lu detail=%lu phase=%lu  last_poll_us=%lu  hardfault_pc=0x%08lx\n",
              (unsigned long)sc1, (unsigned long)(sc1 >> 28), (unsigned long)((sc1 >> 8) & 0xFFFFFu),
              (unsigned long)(sc1 & 0xFFu), (unsigned long)sc2, (unsigned long)sc3);
    ba_printf("[ba] !!!! file 1=uthernet2_net.cpp 2=uthernet2.c 3=network_pump.cpp 4=ServicePoll section 5=udptask.cpp;"
              " detail = source line for lock sites (phase 1 waiting, 2 holding, 3 released)\n");
  }
  ba_drain();
}

void U2_BaEvent(const char *tag, long a, long b) {
  uint64_t t = time_us_64();
  ba_printf("[ba] t=%lu.%06lu ev=%s a=%ld b=%ld cyc=%lu\n", (unsigned long)(t / 1000000u),
            (unsigned long)(t % 1000000u), tag, a, b, (unsigned long)g_ba_cyc);
}

void U2_BaNoteResetServed(int rc) {
  uint32_t lat = timer_hw->timerawl - g_ba_reset_t;
  if (rc)
    ba_rst_ok++;
  else
    ba_rst_skip++;
  if (lat > ba_rst_lat_max)
    ba_rst_lat_max = lat;
  U2_BaEvent(rc ? "reset_served" : "reset_SKIPPED", (long)lat, (long)g_ba_resets);
}

static const char *const ba_cls[8] = {"W0", "W1", "W2", "W3", "R0", "R1", "R2", "R3"};

static void ba_report(void) {
  uint64_t t = time_us_64();
  ba_printf("[ba] === t=%lu.%03lus cyc=%lu polls=%lu\n", (unsigned long)(t / 1000000u),
            (unsigned long)((t % 1000000u) / 1000u), (unsigned long)g_ba_cyc, (unsigned long)ba_polls);
  ba_printf("[ba] core0 gaps: max=%lu us  >5ms=%lu >20ms=%lu >100ms=%lu >1s=%lu\n",
            (unsigned long)ba_gap_max, (unsigned long)ba_gap5, (unsigned long)ba_gap20,
            (unsigned long)ba_gap100, (unsigned long)ba_gap1000);
  ba_printf("[ba] listener: RXSTALL=%lu  fifo@start %lu/%lu/%lu/%lu/%lu  fifo@publish %lu/%lu/%lu/%lu/%lu\n",
            (unsigned long)ba_rxstall, (unsigned long)g_ba_lvl_b[0], (unsigned long)g_ba_lvl_b[1],
            (unsigned long)g_ba_lvl_b[2], (unsigned long)g_ba_lvl_b[3], (unsigned long)g_ba_lvl_b[4],
            (unsigned long)g_ba_lvl_e[0], (unsigned long)g_ba_lvl_e[1], (unsigned long)g_ba_lvl_e[2],
            (unsigned long)g_ba_lvl_e[3], (unsigned long)g_ba_lvl_e[4]);
  for (unsigned c = 0; c < 8; c++) {
    uint32_t n = 0;
    for (unsigned b = 0; b < 7; b++)
      n += g_ba_h_hist[c][b];
    if (!n)
      continue;
    ba_printf("[ba] %s n=%lu H %lu/%lu/%lu/%lu/%lu/%lu/%lu max=%lu | T %lu/%lu/%lu/%lu/%lu/%lu/%lu max=%lu\n",
              ba_cls[c], (unsigned long)n, (unsigned long)g_ba_h_hist[c][0], (unsigned long)g_ba_h_hist[c][1],
              (unsigned long)g_ba_h_hist[c][2], (unsigned long)g_ba_h_hist[c][3],
              (unsigned long)g_ba_h_hist[c][4], (unsigned long)g_ba_h_hist[c][5],
              (unsigned long)g_ba_h_hist[c][6], (unsigned long)g_ba_h_max[c],
              (unsigned long)g_ba_t_hist[c][0], (unsigned long)g_ba_t_hist[c][1],
              (unsigned long)g_ba_t_hist[c][2], (unsigned long)g_ba_t_hist[c][3],
              (unsigned long)g_ba_t_hist[c][4], (unsigned long)g_ba_t_hist[c][5],
              (unsigned long)g_ba_t_hist[c][6], (unsigned long)g_ba_t_max[c]);
  }
  ba_printf("[ba] Sn_CR write T %lu/%lu/%lu/%lu/%lu/%lu/%lu max=%lu\n", (unsigned long)g_ba_cmdw_h[0],
            (unsigned long)g_ba_cmdw_h[1], (unsigned long)g_ba_cmdw_h[2], (unsigned long)g_ba_cmdw_h[3],
            (unsigned long)g_ba_cmdw_h[4], (unsigned long)g_ba_cmdw_h[5], (unsigned long)g_ba_cmdw_h[6],
            (unsigned long)g_ba_cmdw_max);
  ba_printf("[ba] reset: n=%lu served=%lu SKIPPED=%lu lat_max=%lu us\n", (unsigned long)g_ba_resets,
            (unsigned long)ba_rst_ok, (unsigned long)ba_rst_skip, (unsigned long)ba_rst_lat_max);
  ba_printf("[ba] missed_command why1..7: %lu/%lu/%lu/%lu/%lu/%lu/%lu  clamp=%lu\n",
            (unsigned long)g_ba_missed[1], (unsigned long)g_ba_missed[2], (unsigned long)g_ba_missed[3],
            (unsigned long)g_ba_missed[4], (unsigned long)g_ba_missed[5], (unsigned long)g_ba_missed[6],
            (unsigned long)g_ba_missed[7], (unsigned long)g_ba_clamp);
  ba_printf("[ba] served!=word-byte (stale/shifted read) loc0..3: %lu/%lu/%lu/%lu  triggers cr3=%lu miss=%lu "
            "clamp=%lu pk3=%lu pkoth=%lu suppressed=%lu uartdrop=%lu\n",
            (unsigned long)g_ba_pkmis[0], (unsigned long)g_ba_pkmis[1], (unsigned long)g_ba_pkmis[2],
            (unsigned long)g_ba_pkmis[3], (unsigned long)g_ba_trig_n[U2_BA_T_CR3],
            (unsigned long)g_ba_trig_n[U2_BA_T_MISSED], (unsigned long)g_ba_trig_n[U2_BA_T_CLAMP],
            (unsigned long)g_ba_trig_n[U2_BA_T_PK3], (unsigned long)g_ba_trig_n[U2_BA_T_PKOTHER],
            (unsigned long)g_ba_trig_suppressed, (unsigned long)ba_tx_drop);
  {
    extern uint32_t U2_BaStaIp(void);
    extern uint32_t U2_BaSipr(void);
    uint32_t sta = U2_BaStaIp(), sipr = U2_BaSipr(); /* both little-endian byte order = a.b.c.d */
    ba_printf("[ba] ip: pico STA %u.%u.%u.%u  SIPR(6502) %u.%u.%u.%u\n", (unsigned)(sta & 255u),
              (unsigned)((sta >> 8) & 255u), (unsigned)((sta >> 16) & 255u), (unsigned)(sta >> 24),
              (unsigned)(sipr & 255u), (unsigned)((sipr >> 8) & 255u), (unsigned)((sipr >> 16) & 255u),
              (unsigned)(sipr >> 24));
  }
  ba_printf("[ba] uart backlog=%lu bytes (event t= is real time; lines print late when this is large)\n",
            (unsigned long)(ba_tx_w - ba_tx_r));
  ba_printf("[ba] xip: acc=%lu hit=%lu\n", (unsigned long)xip_ctrl_hw->ctr_acc,
            (unsigned long)xip_ctrl_hw->ctr_hit);
}

static void ba_dump_ring(void) {
  uint32_t th = g_ba_trig_head;
  uint32_t first = th - U2_BA_PRE;
  uint32_t last = th + U2_BA_POST; /* exclusive */
  if (g_ba_head < last)
    last = g_ba_head;
  uint32_t tt = g_ba_ring[th & (U2_BA_RING_N - 1u)].t;
  ba_dumps++;
  ba_printf("[ba] --- RING DUMP #%lu trigger=%lu (1=CR$03 2=missed 3=clamp 4=served!=word@data 5=served!=word@regs)\n",
            (unsigned long)ba_dumps, (unsigned long)g_ba_trig_code);
  for (uint32_t i = first; (int32_t)(i - last) < 0; i++) {
    const u2_ba_ev_t *e = &g_ba_ring[i & (U2_BA_RING_N - 1u)];
    int32_t dt = (int32_t)(e->t - tt);
    ba_printf("[ba] %c%+6ld us %c%u wd=%02x sv=%02x got=%02x pk=%02x m=%02x A=%04x h=%u T=%u q=%u/%u\n",
              i == th ? '>' : ' ', (long)dt, (e->bus & 0x10) ? 'R' : 'W', (unsigned)(e->bus & 3u),
              e->wdata, e->served, e->gotby, e->peek, e->mode, e->adr, e->h, e->tot, e->q & 15u,
              e->q >> 4);
    ba_drain();
  }
  ba_printf("[ba] --- end dump\n");
}

/* Print queued lifecycle trace entries (core 0). */
static void ba_trace_drain(void) {
  uint32_t head = __atomic_load_n(&g_ba_tr_head, __ATOMIC_ACQUIRE);
  if ((uint32_t)(head - ba_tr_tail) > U2_BA_TR_N) {
    uint32_t keep = head - U2_BA_TR_N;
    ba_tr_lost += keep - ba_tr_tail;
    ba_tr_tail = keep;
  }
  uint64_t now64 = time_us_64();
  while (ba_tr_tail != head) {
    u2_ba_tr_t *e = &g_ba_tr[ba_tr_tail & (U2_BA_TR_N - 1u)];
    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != ba_tr_tail + 1u)
      break; /* still being written */
    uint32_t t32 = e->t;
    const char *tag = e->tag;
    int32_t a = e->a, b = e->b, c = e->c;
    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != ba_tr_tail + 1u)
      break;
    uint64_t full = (now64 & ~0xFFFFFFFFull) | t32;
    if (full > now64)
      full -= 0x100000000ull;
    ba_printf("[ba] t=%lu.%06lu tr=%s a=%ld b=%ld c=%ld cyc=%lu\n", (unsigned long)(full / 1000000u),
              (unsigned long)(full % 1000000u), tag, (long)a, (long)b, (long)c, (unsigned long)g_ba_cyc);
    ba_tr_tail++;
    if (BA_TXN - (ba_tx_w - ba_tx_r) < 512u) {
      ba_drain();
      if (BA_TXN - (ba_tx_w - ba_tx_r) < 512u)
        break; /* UART behind; resume next poll */
    }
  }
}

/* Fatal path: print every queued trace entry, ignoring ring space and UART lag. */
static void ba_trace_drain_fatal(void) {
  for (int guard = 0; guard < 4; guard++) {
    ba_trace_drain();
    ba_fatal_flush_ring();
  }
}

void U2_BaCore0Poll(void) {
  if (!ba_up)
    return;
  uint32_t now = time_us_32();
  ba_polls++;
  if (ba_have_last) {
    uint32_t gap = now - ba_last_service_us;
    if (gap > ba_gap_max)
      ba_gap_max = gap;
    if (gap >= 1000000u)
      ba_gap1000++;
    if (gap >= 100000u)
      ba_gap100++;
    if (gap >= 20000u) {
      ba_gap20++;
      if (ba_gap_events++ < 60u)
        U2_BaEvent("core0_away_us", (long)gap, (long)(g_ba_cyc - ba_last_service_cyc));
    }
    if (gap >= 5000u)
      ba_gap5++;
  }
  if (!ba_wdog_on) {
    watchdog_hw->scratch[0] = U2_BA_WDOG_MAGIC;
    watchdog_enable(U2_BA_WDOG_MS, true);
    ba_wdog_on = true;
  }
  watchdog_update();
  watchdog_hw->scratch[2] = now;
  U2_BaStage((4u << 28) | 1u);
  ba_have_last = true;
  ba_last_service_us = now;
  ba_last_service_cyc = g_ba_cyc;

  /* Wi-Fi bring-up timeline (section 1fa): log every CYW43_LINK_* change, sampled every ~20 ms. */
  {
    static int ba_link_last = -100;
    static uint32_t ba_link_next_us;
    if ((int32_t)(now - ba_link_next_us) >= 0) {
      ba_link_next_us = now + 20000u;
      extern int U2_Net_LinkStatusRaw(void);
      extern int U2_Net_LinkReady(void);
      int ls = U2_Net_LinkStatusRaw() * 2 + (U2_Net_LinkReady() ? 1 : 0);
      if (ls != ba_link_last) {
        U2_BaEvent("link", (long)(ls >> 1), (long)(ls & 1));
        ba_link_last = ls;
      }
    }
  }

  if (pio0->fdebug & (1u << SM_LISTENER)) {
    pio0->fdebug = (1u << SM_LISTENER);
    ba_rxstall++;
    if (ba_rxstall_events++ < 20u)
      U2_BaEvent("RXSTALL(listener dropped a cycle)", (long)ba_rxstall, 0);
  }

  ba_trace_drain();
  if (g_ba_frozen) {
    ba_dump_ring();
    g_ba_frozen = 0;
  }
  if ((int32_t)(now - ba_next_report_us) >= 0) {
    ba_next_report_us = now + 10u * 1000u * 1000u;
    ba_report();
    if (ba_tr_lost)
      ba_printf("[ba] trace entries lost: %lu\n", (unsigned long)ba_tr_lost);
  }
  ba_drain();
}

#endif /* U2_BUS_AUDIT */

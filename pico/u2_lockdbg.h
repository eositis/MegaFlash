/* lwIP lock call-site breadcrumbs for the audit image (docs section 1ex).
 * Define U2_FILE_ID (1..5, see u2_busaudit.h) before including. With U2_BUS_AUDIT=0 these are
 * exactly cyw43_arch_lwip_begin()/end(). */
#ifndef U2_LOCKDBG_H
#define U2_LOCKDBG_H

#include "pico/cyw43_arch.h"

#ifndef U2_BUS_AUDIT
#define U2_BUS_AUDIT 0
#endif

#if U2_BUS_AUDIT
#include "hardware/structs/watchdog.h"
/* Same register as U2_LD_STAGE(); declared here so including this header does not pull a2bus.h. */
#define U2_LD_STAGE(id) (watchdog_hw->scratch[1] = (id))
#define U2_LWIP_BEGIN()                                                                          \
  do {                                                                                           \
    U2_LD_STAGE(((uint32_t)U2_FILE_ID << 28) | ((uint32_t)__LINE__ << 8) | 1u);                   \
    cyw43_arch_lwip_begin();                                                                     \
    U2_LD_STAGE(((uint32_t)U2_FILE_ID << 28) | ((uint32_t)__LINE__ << 8) | 2u);                   \
  } while (0)
#define U2_LWIP_END()                                                                            \
  do {                                                                                           \
    cyw43_arch_lwip_end();                                                                       \
    U2_LD_STAGE(((uint32_t)U2_FILE_ID << 28) | ((uint32_t)__LINE__ << 8) | 3u);                   \
  } while (0)
#define U2_MARK() U2_LD_STAGE(((uint32_t)U2_FILE_ID << 28) | ((uint32_t)__LINE__ << 8) | 0x10u)
#else
#define U2_MARK() ((void)0)
#define U2_LWIP_BEGIN() cyw43_arch_lwip_begin()
#define U2_LWIP_END() cyw43_arch_lwip_end()
#endif

#endif

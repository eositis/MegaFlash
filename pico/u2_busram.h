/**
 * Placement macro for code reachable from U2_HandleBusAccess (docs section 1ev).
 *
 * Everything the Uthernet II bus core (core 1) can execute while serving a $C0C4-$C0C7 cycle
 * must live in SRAM: flash is fetched through the XIP cache that core 0 (lwIP, CYW43, STL
 * containers) thrashes, and a miss stalls the bus core past the next 6502 access.
 * pico/tools/check_bus_ram.py fails the build if the call tree reaches any flash function.
 *
 * RP2040 keeps the old behaviour: its RAM is almost fully used and U2 is run on RP2350.
 */
#ifndef U2_BUSRAM_H
#define U2_BUSRAM_H

#include "pico/platform.h"

#ifndef PICO_RP2040
#define U2_BUS_RAM(fn) __time_critical_func(fn)
#else
#define U2_BUS_RAM(fn) fn
#endif

#endif /* U2_BUSRAM_H */

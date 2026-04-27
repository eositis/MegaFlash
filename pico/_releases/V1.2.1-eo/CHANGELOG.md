# V1.2.1-eo

**1.2 series:** V1.1.24-eo was the final release in the 1.1.x line. Development from **V1.2.0-eo** onward uses the **1.2.x** series, focused on **Uthernet II emulation**, **com port**, and **imagewriter emulation**.

## Uthernet II / ip65 fixes

- **Fixed W5100 TX pointer register reads:** `SN_TX_RD0/1` and `SN_TX_WR0/1` now return the real emulated register bytes instead of incorrectly falling through `RX_RSR` handling. This fixes host-side pointer corruption that produced malformed MACRAW TX frames.
- **Fixed MACRAW RX length prefix compatibility:** MACRAW RX enqueue now writes wire length as `len+2` in the 2-byte prefix (W5100 convention), avoiding frame truncation in clients that subtract 2 from the prefix.
- **DHCP payload mutation rollback for compatibility:** Outbound DHCP handling now avoids rewriting BOOTP payload fields (`chaddr` / client-id option path) and keeps only Ethernet source MAC normalization, reducing client-side validation mismatch risk.

## Uthernet networking stability and diagnostics

- **Core-affinity stabilization:** Uthernet network polling/transmit path remains constrained to core 0 service flow to avoid CYW43/lwIP `async_context` wrong-core panics.
- **Low-noise Ethernet tracing support:** Header-level Uthernet tracing (`[u2eth]`) and optional checkpoint/bus diagnostics were used to validate frame flow while keeping logs manageable.
- **Additional MACRAW instrumentation:** Monitor/tap events were extended for SEND pointer progression and queue-stage payload inspection to isolate pre-core0 vs post-core0 corruption.

## Notes

- This release includes substantial Uthernet emulation correctness work and debugability improvements used to validate ip65/Telnet/Contiki/ADTPro paths.

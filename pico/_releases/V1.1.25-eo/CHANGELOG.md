# V1.1.25-eo

## Maintenance (1.1.x)

- **DNS:** `CUDPTask::DNSLookup` — arm `dnsTimeout` before `dns_gethostbyname()`, and clear `dnsTimeout` on immediate result paths (`ERR_OK`, `ERR_ARG`, other non-`ERR_INPROGRESS`). Reduces intermittent DNS failures when lwIP delivers the resolver callback before timeout state is set.

## Firmware

- **Numeric:** `0x001d`
- **Build date:** 27-Mar-2026 (release packaging)

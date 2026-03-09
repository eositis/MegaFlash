# Changelog — V1.1.9-eo (07-Mar-2025)

## Hardware / GPIO

- **nDEVSEL pull removed:** Internal pull-up on nDEVSEL (GPIO 20) has been removed in both RP2040 and RP2350 PIO inits. The line is now driven only by the Apple bus / GAL; no internal pull. (`a2bus_rp2040.pio`, `a2bus_rp2350.pio`: `gpio_pull_up(nDEVSEL_GPIO)` → `gpio_set_pulls(nDEVSEL_GPIO, false, false)`.)

---

*Previous release: V1.1.8-eo — C0xx concurrent ranges, U2 C0C4–C0C7, read-back fix, C0C4 LED, A2/A3 and nDEVSEL pull configuration.*

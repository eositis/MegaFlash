# V1.1.11-eo

## TFTP RX: buffer incoming traffic during flash write

- **Incoming TFTP DATA buffering:** While the TFTP RX task is blocked in flash write (especially during the ~220–250 ms sector erase every 16 blocks), incoming DATA packets are now received and queued instead of being dropped or causing server timeouts.
- **Flash yield callback:** The flash layer calls an optional yield callback (~1 ms) during `WaitUntilBusyClear()`. TFTP RX registers `tftp_network_yield()` so `cyw43_arch_poll()` runs during erase and UDP can be received.
- **RX queue:** Up to 16 DATA packets are stored in a circular queue when they arrive during a blocking write. The event loop drains one queued packet per iteration so they are processed in order after the write returns.
- **Effect:** Fewer timeouts and retries during TFTP download; transfer can proceed more smoothly when the server sends the next block while the Pico is erasing a sector.

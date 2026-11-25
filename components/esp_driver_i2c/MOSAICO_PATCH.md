# Local override of ESP-IDF `esp_driver_i2c`

Copied from `$IDF_PATH/components/esp_driver_i2c` (IDF 6.2) with one change in
`i2c_master.c` (`s_i2c_send_commands`): `contains_read` is cleared at the start
of every transaction.

Why: a read that ended early (NACK or timeout, as when the camera driver probes
for sensors) left `contains_read` true. The next `i2c_master_probe()` (the
module manager probes the camera slot periodically) then ran the receive
handler on its data-less ops and stored to NULL+n inside the ISR: a store access
fault in `i2c_ll_read_rxfifo`, which crashed the board when the camera opened.

The first fix was not enough (the crash came back, 2026-11-25): a read that
timed out also leaves an interrupt pending, which fired as soon as the next
probe enabled interrupts, before `s_i2c_send_commands` reset the flag. Now
the flag and pending interrupts are cleared before interrupts are enabled
(transfers and probes), the stack ops are forgotten when a transfer ends, and
the ISR never reads into a missing buffer.

Drop this directory once ESP-IDF fixes it upstream.

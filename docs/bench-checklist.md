# Bench acceptance

Use [the handoff](../HAMR_Caster_ESP32_Codex_Handoff.md) for exact connections and component requirements. Record the date, board role, firmware version, configuration, and observed results for each run. Unchecked items below are **not yet hardware-verified**.

## Before power

- [ ] Confirm each node has its own battery, UBEC, and local ground; no wires cross between rotating nodes.
- [ ] Measure the UBEC output at 5.0 V before connecting it. Verify diode orientation and leave BAT pads unused.
- [ ] Compare every encoder connector and buffer pin to the handoff. H1/H2 buffer VCC is 3.3 V; CENTER CS pull-up is to 3.3 V.
- [ ] Check the buffer supply bypass capacitor and grounded unused inputs.
- [ ] Disconnect battery/UBEC before attaching computer USB. For encoder tests, verify a deliberate single-source supply arrangement; do not assume USB powers the encoder.
- [ ] Label the physical boards H1, H2, and CENTER and flash their matching roles.

## Startup and reception

- [ ] Hold the mechanism stationary during startup. Verify role, GPIOs, reset reason, firmware version, and PPR assumption in the startup log.
- [ ] Verify each node boots and acquires without a USB host.
- [ ] Confirm all three nodes reach the intended computer and that the logger creates usable JSONL and CSV files.
- [ ] Confirm metadata matches actual signs, zeros, rates, PPR settings, and verification status.

## H1 and H2 — test independently

- [ ] Turn slowly in each direction; verify count and velocity reverse smoothly. Choose the physical positive direction for each mounting and set its sign.
- [ ] Rotate monotonically through multiple full turns. Compare consecutive same-edge Z observations. A 2048 PPR encoder should produce 8192 x4 counts per revolution; determine the actual setting before marking PPR verified.
- [ ] Avoid reversing near Z during the PPR measurement. The selected edge is rising; its timestamp/count describe GPIO interrupt service, with latency not yet measured. Use only observations marked index-valid.
- [ ] Cross positive and negative PCNT hardware limits repeatedly; verify no artificial jumps or resets in the application count.
- [ ] Confirm each Z event increments index diagnostics without resetting raw count.
- [ ] Document any relative count offset. Reboot and confirm phase-reference validity remains false: this version has no runtime physical-reference calibration for the incremental encoders.
- [ ] Check the configured glitch-filter width against actual PPR and maximum RPM. Do not use mechanical-switch debounce values.

## CENTER

- [ ] Check raw SPI words, parity results, valid range 0–16383, and direction against slow known motion.
- [ ] Cross 16383 → 0 and 0 → 16383; verify the absolute-angle wrap and continuous unwrapped increments while the configured rate bound is satisfied.
- [ ] Verify a disconnected or invalid sensor produces invalid samples, not a valid zero or an unmarked reused value.
- [ ] Test a half-turn jump and a long/invalid-read gap; verify unwrap continuity becomes uncertain when the motion bound no longer resolves the ambiguity.
- [ ] Confirm physical sign, software zero, and maximum angular-rate configuration. Do not count turns occurring while power is off.

## Timing, loss, and restart

- [ ] Interrupt Wi-Fi, continue moving, then restore it. Verify counting/acquisition continues and host records expose missing sample sequences.
- [ ] Induce queue saturation in a controlled test and distinguish queue drops, acquisition overruns, SPI errors, and transport failures.
- [ ] Restart one node. Verify its new boot ID starts a separate session and does not imply continuous integration with the previous boot.
- [ ] Confirm all node clocks remain marked unsynchronized until an actual offset/drift calibration has been supplied. Receiver arrival time alone is not a synchronization measurement.
- [ ] Measure real sampling/transmission behavior and loss at expected operating speeds. Compiling and host unit tests do not establish electrical or real-time performance.

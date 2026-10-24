# Bench acceptance

Use [the handoff](../HAMR_Caster_ESP32_Codex_Handoff.md) for exact connections and component requirements. Record the date, wheel, board role, device_id, firmware version, configuration, and observed results for each run. Unchecked items below are **not yet hardware-verified**.

## Before power

- [ ] Confirm each node has its own battery, UBEC, and local ground; no wires cross between rotating nodes.
- [ ] Measure the UBEC output at 5.0 V before connecting it. Verify diode orientation and leave BAT pads unused.
- [ ] Compare every encoder connector and buffer pin to the handoff. H1/H2 buffer VCC is 3.3 V; CENTER CS pull-up is to 3.3 V.
- [ ] Check the buffer supply bypass capacitor and grounded unused inputs.
- [ ] Disconnect battery/UBEC before attaching computer USB. For encoder tests, verify a deliberate single-source supply arrangement; do not assume USB powers the encoder.
- [ ] Label all six physical boards front_left/front_right + H1/H2/CENTER and flash their matching profiles.

## Startup and reception

- [ ] Hold the mechanism stationary during startup. Verify wheel, role, device_id, Pi destination, GPIOs, reset reason, firmware version, and PPR assumption in the startup log.
- [ ] Verify each node boots and acquires without a USB host.
- [ ] Confirm all six nodes reach the intended computer and that the logger creates usable JSONL and CSV files.
- [ ] Confirm metadata matches actual signs, zeros, rates, PPR settings, and verification status.

## Wheel identity and Pi storage

- [ ] Record every board's startup device_id in the matching Pi inventory entry; require all six IDs before recording.
- [ ] Move only front-left H1, then H2, then CENTER; only the matching `/caster_encoder/front_left/<ROLE>/joint_states` measurement should change. Repeat for front-right.
- [ ] Confirm every left packet has `wheel_id: front_left` and the expected MAC, and every right packet has `wheel_id: front_right` and its own MAC.
- [ ] Reboot one front-left board while front-right runs; the right streams remain live, and the restarted left device gets a separate boot session.
- [ ] Check `ros2 bag info` for all 12 wheel topics, then analyze both raw packet topics and inspect wheel/device columns in the exports.
- [ ] Confirm the Pi rejects a wrong wheel on a listener and an unexpected device ID. Restore the correct profile/inventory before acquisition.

## H1 and H2 — test independently

- [ ] Turn slowly in each direction; verify count and velocity reverse smoothly. Choose the physical positive direction for each mounting and set its sign.
- [ ] Rotate monotonically through multiple full turns. Compare consecutive same-edge Z observations. A 2048 PPR encoder should produce 8192 x4 counts per revolution; determine the actual setting before marking PPR verified.
- [ ] Avoid reversing near Z during the PPR measurement. The selected edge is rising; its timestamp/count describe GPIO interrupt service, with latency not yet measured. Use only observations marked index-valid.
- [ ] Cross positive and negative PCNT hardware limits repeatedly; verify no artificial jumps or resets in the application count.
- [ ] Confirm each Z event increments index diagnostics without resetting raw count.
- [ ] Document any relative count offset. Reboot and confirm phase-reference validity remains false: this version has no runtime physical-reference calibration for the incremental encoders.
- [ ] Check the configured glitch-filter width against actual PPR and maximum RPM. Do not use mechanical-switch debounce values.
- [ ] Measure a known rotation over ESP acquisition time, not UDP arrival time. At verified 2048 PPR, +8192 counts in 1 second gives +2π rad displacement and +6.283185 rad/s average velocity with sign +1 (60 RPM). Negative direction/sign reverses both. The first sample has no valid velocity; the low-pass output has deliberate lag.

## CENTER

- [ ] Check raw SPI words, parity results, valid range 0–16383, and direction against slow known motion.
- [ ] Cross 16383 → 0 and 0 → 16383; verify the absolute-angle wrap and continuous unwrapped increments while the configured rate bound is satisfied.
- [ ] Verify a disconnected or invalid sensor produces invalid samples, not a valid zero or an unmarked reused value.
- [ ] Test a half-turn jump and a long/invalid-read gap; verify unwrap continuity becomes uncertain when the motion bound no longer resolves the ambiguity.
- [ ] Confirm physical sign, software zero, and maximum angular-rate configuration. Do not count turns occurring while power is off.
- [ ] Begin with wrapped position (`absolute_angle_rad` / `calibrated_alpha_rad`). If `CENTER_MAX_RPM_VERIFIED` is false, missing unwrapped angle and velocity are expected; do not mark the bound verified just to obtain a number.
- [ ] For live velocity, establish and document a conservative maximum physically possible RPM, set `CENTER_MAX_RPM` and its verified flag in this wheel's private header, and reflash this CENTER. The valid-read interval must satisfy `max_rpm / 60 × 16384 × interval_seconds + 2 < 8192`. At 600 RPM this is below approximately 49.988 ms; target sampling is 5 ms.
- [ ] After enabling the verified bound, compare one revolution in 1 second against about 6.283185 rad/s (60 RPM), reversing sign for the opposite direction. Retain serial continuity-loss warnings; after an ambiguous interval, local velocity can recover but unwrapped position stays unavailable until reboot.
- [ ] For provisional slow bench estimates before rate-bound verification, run `python3 host/center_velocity.py PATH/TO/samples.csv --wheel front_left` (or `front_right`). Inspect status/gap/identity columns. This output assumes less than half a turn between endpoints; it does not establish a physical rate bound or change firmware flags.

## Timing, loss, and restart

- [ ] Interrupt Wi-Fi, continue moving, then restore it. Verify counting/acquisition continues and host records expose missing sample sequences.
- [ ] Induce queue saturation in a controlled test and distinguish queue drops, acquisition overruns, SPI errors, and transport failures.
- [ ] Restart one node. Verify its new boot ID starts a separate session and does not imply continuous integration with the previous boot.
- [ ] Confirm all node clocks remain marked unsynchronized until an actual offset/drift calibration has been supplied. Receiver arrival time alone is not a synchronization measurement.
- [ ] Measure real sampling/transmission behavior and loss at expected operating speeds. Compiling and host unit tests do not establish electrical or real-time performance.

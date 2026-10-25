# Validation record

## Explicit storage discard, firmware 0.3.1 (2026-10-25)

- `CJSON_SOURCE_DIRECTORY=/tmp/hamr-cjson-8f2beb57 ./scripts/test.sh` passed
  encoder-math assertions and all 36 Python tests, including compilation of the
  actual recorder and all six protocol profiles. cJSON source matches ESP-IDF
  v5.4.2's submodule commit `8f2beb57ddad1f94bed899790b00f46df893ccac`.
- Native recorder tests cover recovery from `invalid_header`, explicit deletion
  of unread/prepared contents, omitted/false/mistyped flags, required guard types,
  device/boot/retained-run conflicts, active/acquiring/finalizing refusal, maximum
  run IDs, same-run retry preservation (including terminal errors and reboots),
  and failed journal commits/torn replacement headers with no erasure on reboot.
- Guarded stop tests confirm that wrong device/boot or malformed guard fields
  cannot stop an active run, matching guards stop and drain it, and legacy stops
  without guards retain their existing behavior.
- The preparation journal test substitute now models staged versus committed
  intent, so failure to commit an intent preserves the original log and failure
  to clear an intent preserves the interrupted-preparation recovery path.

These are native software checks. No ESP-IDF target build, device flashing,
network mutation, or hardware flash erase was performed for this change.

## Commanded local recording, firmware 0.3.0 (2026-10-24)

- Final front-left CENTER application built and linked successfully with the
  pinned ESP-IDF v5.4.2 toolchain. Binary size is `0xea9a0` bytes, within the
  unchanged 1 MiB application partition. The generated partition table includes
  the `caster_log` partition with capacity for 30,720 full sample records.
- Compiler checks exercised the firmware translation units for all six
  wheel/role profiles. The right-wheel checks used its example header because
  this checkout does not currently contain `config/front_right.local.h`.
- Native encoder-math assertions and actual cJSON protocol serialization tests
  passed. New tests compile the real recorder implementation against deterministic
  SDK/flash substitutes and exercise capture boundaries, idempotent commands,
  capacity/overflow, corrupt/torn writes, preparation interruption, reboot
  recovery, unread-log protection, and download acknowledgement.
- The companion Pi/analyzer regression run passed 197 tests, with 11 existing
  environment-dependent skips. After adding acquisition-window validation, the
  final focused recorder-format/client run passed all 75 tests. These include
  loopback HTTP downloads, partial failure/retry, identity mismatches, clock
  drift/steps/reboots, corrupted downloads, and durable files before ACK.
- An isolated ament CMake installation under WSL with ROS Humble succeeded;
  `ros2 run hamr_bringup caster_recording --help` resolved the installed modules
  and configuration. No discovery or physical recording was started.

These are software checks. The six boards were not flashed or exercised as part
of this implementation. Actual flash-write acquisition intervals, Wi-Fi start/
stop behavior, power-loss recovery, and clock-estimate accuracy still require
bench qualification. The original private configuration and the user's serial
debug interval were preserved. See [the operator guide](../../hamr_control/docs/LOCAL_CASTER_RECORDING.md).

## Earlier firmware 0.2.1 validation

Software checks run on 2026-10-24. Firmware version: `0.2.1`, telemetry version: `2`.

Upstream `origin/main` was fast-forwarded from `58381b3` through `4ca21ea` to
`eceb24e` and the local two-wheel work was reapplied. Conflicts were resolved;
the latest CENTER GPIO/CS setup, acquisition startup ordering, and first-loss
diagnostics are retained. Private configuration files were compared byte for
byte with the pre-merge backup and are unchanged. Local implementation changes
remain uncommitted; the pre-merge stash and recovery backup were retained.

## Executed

- All six wheel/role firmware builds completed with ESP-IDF v5.4.2, with no compiler warnings/errors. Final builds include the wheel-specific private settings and audited network metadata. Compile-command checks confirmed each selected wheel configuration, wheel index, node ID 1/2/3, and `-fwrapv`. All application binaries fit the 1 MiB partition; artifacts are in `build/<wheel>/<ROLE>/`.

  | Wheel | Role | Application bytes | Result |
  |---|---|---:|---|
  | front_left | H1 | 885344 | Passed |
  | front_left | H2 | 885360 | Passed |
  | front_left | CENTER | 886080 | Passed |
  | front_right | H1 | 885344 | Passed |
  | front_right | H2 | 885360 | Passed |
  | front_right | CENTER | 886080 | Passed |

- `./scripts/test.sh`: portable C++ encoder-math assertions and **35 Python tests discovered: 34 passed, one optional serializer test skipped** in the plain host environment. Coverage includes loss/duplicate/recovery, malformed packet rejection, exact 64-bit values, actual subprocess UDP recording, same-role/same-boot traffic from both wheels, reboots, device-assignment conflicts, wheel filters, and identity-scoped clock mappings.
- C++ math checks also passed with UndefinedBehaviorSanitizer. Known-speed tests cover 60 RPM incremental and CENTER motion, two complete absolute revolutions, both signs, offsets, counter rollover, single-count precision above 2^53, and opposite signed 64-bit extremes. Existing parity, ambiguous-gap, half-turn, excessive-motion, velocity-recovery and first-loss diagnostics checks pass.
- The upstream offline CENTER estimator now separates wheel/MAC/boot histories, rejects conflicting assignments, retains identity in its output, and supports `--wheel`. Tests exercise identical boot/sequence values on left/right and confirm wrong/partial identities never contaminate accepted velocity endpoints.
- Windows flashing tests cover all six profiles, correct build arguments/artifact paths, a dry run with no build or serial access, and rejection of a mismatched CMake profile. Actual Windows serial flashing was not performed. Windows logger/relay recording helpers now select a wheel explicitly; Python syntax checks passed, but the Windows relay path was not hardware-tested here.
- **171 focused companion `hamr_control` tests passed** using the available ROS Humble test overlay. This includes six simulated UDP devices through two real ROS bridges, the production bag recorder, MCAP, and separate wheel CSV export, with wrong-wheel/MAC/IP rejection and isolated reboot handling. Matplotlib emitted an unrelated optional Axes3D availability warning.
- Build-profile tests exercised six isolated build paths and compiled all six configurations independently, checking role, wheel, UDP port, signs/PPR, and per-board static IPv4 selection. The helper rejects attempts to override its selected wheel, role, or build directory.
- `./scripts/test_protocol.sh` separately **passed** against the pinned SDK's actual cJSON library and the actual `main/protocol.cpp` for all six profiles. Only the SDK-version query was stubbed. The harness verifies identity in every sample/metadata packet, audited network configuration, full signed/unsigned 64-bit values, finite double limits, NaN/infinity to null, and payloads within the 1472-byte UDP payload budget for a standard 1500-byte IPv4 MTU.
- Both private wheel configuration files are ignored by Git. The current files target Pi `10.42.0.14`, left port 5005/right port 5006, gateway `10.42.0.1`, `/24` netmask, and DHCP for the ESP32s. Both use unverified 2048 PPR and unverified 600 RPM bounds. These are user settings, preserved through the merge; none was marked physically verified by this validation.
- Build helper/test shell syntax and `git diff --check` passed. ESP-IDF is pinned to **v5.4.2**, including its container digest. Source inspection confirms the initial DHCP state can be stopped before applying the optional static IP settings.

## Hardware checks still required

No controller USB serial device was present, so **nothing was flashed or tested on physical hardware**. Wiring, supplies, actual PPR, signs/physical zeros, Z ISR latency, counter-limit behavior on the chip, SPI timing/electrical behavior, Wi-Fi throughput/reconnect, queue saturation, and battery operation remain unverified. Follow [the bench checklist](bench-checklist.md).

The counter snapshot rejects a read if a PCNT limit interrupt is pending, rather than publishing an inconsistent count. A rejected sample has null raw count/angles on the wire; a Z event at that instant retains its event/time but clears `index_valid`. A gap exceeding 26 seconds between successful count snapshots latches hemisphere continuity uncertain. Z keeps the latest event and a cumulative event count; `index_queue_drops` is reserved and stays zero because there is no Z event queue.

CENTER's `absolute_angle_rad` and `calibrated_alpha_rad` remain available after valid parity checks. With the default unverified speed bound, unwrapped `angle_rad` and velocity are null. Establish a conservative physical upper speed bound before enabling those calculations. An ambiguous gap latches unwrapped continuity uncertain for the rest of that boot; later short valid intervals may yield valid velocity again.

Physical phase is always unverified at H1/H2 startup; runtime physical-reference acquisition is future work. No timing alignment precision is claimed. Host storage does not recover measurements lost while the receiver or radio is unavailable.

## Implementation references

The original [handoff](../HAMR_Caster_ESP32_Codex_Handoff.md) remains the wiring authority. APIs and timing were checked against [ESP-IDF v5.4.2 PCNT documentation](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32c6/api-reference/peripherals/pcnt.html), the [pinned PCNT driver source](https://github.com/espressif/esp-idf/blob/v5.4.2/components/esp_driver_pcnt/src/pulse_cnt.c), [Seeed's XIAO ESP32-C6 pin map and RF controls](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/), and the [AMT25 manufacturer datasheet](https://www.mouser.lt/datasheet/2/1628/amt25_v-3569567.pdf).

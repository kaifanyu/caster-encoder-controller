# Validation record

Software checks run on 2026-10-08. Firmware version: `0.1.0`.

## Executed

- All three role builds completed successfully with ESP-IDF v5.4.2, with no compiler warnings/errors. The generated compile commands confirm node IDs 1/2/3, target `esp32c6`, and the required PCNT IRAM settings. Each build includes its application, bootloader, partition table, and flash arguments under `build/<ROLE>/`.

  | Role | Application bytes | Result |
  |---|---:|---|
  | H1 | 882128 | Passed; fits 1 MiB application partition |
  | H2 | 882128 | Passed; fits 1 MiB application partition |
  | CENTER | 882048 | Passed; fits 1 MiB application partition |

- `./scripts/test.sh`: portable C++ math assertions and all **14 Python tests** passed. Math coverage includes AMT25 checkbits for all 16384 positions, specified known words and single-bit corruptions, signed 32-bit counter rollovers in both directions, absolute wrap, speed-bound ambiguity, zero/sign, and velocity filtering.
- C++ math tests also passed with undefined-behavior sanitization enabled.
- Python tests include an actual subprocess UDP loopback recording, exact int64 counts, malformed input rejection, late/duplicate/lost samples, separate reboot sessions, and explicit clock-map validity/precision.
- A separate host harness compiled the actual `main/protocol.cpp` against the pinned SDK's cJSON, with only the SDK-version function stubbed. H1 and CENTER packets passed the actual Python decoder, including extreme signed/unsigned 64-bit values, finite double limits, NaN/infinity to null, invalid readings, and metadata. Largest tested two-sample stress payload: 1152 bytes; this fits a 1500-byte IPv4 MTU without fragmentation. This checks serialization, not physical radio performance.
- Build helper shell syntax and non-root Docker SDK activation passed. ESP-IDF is pinned to **v5.4.2**; the helper pins the container digest too.
- The generated PCNT driver compile command includes `-fwrapv`, required for the defined 32-bit wrap semantics used by application count extension.

## Hardware checks still required

No controller USB serial device was present, so **nothing was flashed or tested on physical hardware**. Wiring, supplies, actual PPR, signs/physical zeros, Z ISR latency, counter-limit behavior on the chip, SPI timing/electrical behavior, Wi-Fi throughput/reconnect, queue saturation, and battery operation remain unverified. Follow [the bench checklist](bench-checklist.md).

The counter snapshot rejects a read if a PCNT limit interrupt is pending, rather than publishing an inconsistent count. A rejected sample has null raw count/angles on the wire; a Z event at that instant retains its event/time but clears `index_valid`. A gap exceeding 26 seconds between successful count snapshots latches hemisphere continuity uncertain. Z keeps the latest event and a cumulative event count; `index_queue_drops` is reserved and stays zero because there is no Z event queue.

CENTER's `absolute_angle_rad` and `calibrated_alpha_rad` remain available after valid parity checks. With the default unverified speed bound, unwrapped `angle_rad` and velocity are null. Establish a conservative physical upper speed bound before enabling those calculations. An ambiguous gap latches unwrapped continuity uncertain for the rest of that boot; later short valid intervals may yield valid velocity again.

Physical phase is always unverified at H1/H2 startup; runtime physical-reference acquisition is future work. No timing alignment precision is claimed. Host storage does not recover measurements lost while the receiver or radio is unavailable.

## Implementation references

The original [handoff](../HAMR_Caster_ESP32_Codex_Handoff.md) remains the wiring authority. APIs and timing were checked against [ESP-IDF v5.4.2 PCNT documentation](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32c6/api-reference/peripherals/pcnt.html), the [pinned PCNT driver source](https://github.com/espressif/esp-idf/blob/v5.4.2/components/esp_driver_pcnt/src/pulse_cnt.c), [Seeed's XIAO ESP32-C6 pin map and RF controls](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/), and the [AMT25 manufacturer datasheet](https://www.mouser.lt/datasheet/2/1628/amt25_v-3569567.pdf).

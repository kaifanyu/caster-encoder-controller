# Telemetry and recordings

Each node joins the configured 2.4 GHz Wi-Fi network and sends one JSON object per UDP datagram to the host (default port **5005**). This needs no fourth ESP32. Wi-Fi sleep is disabled during acquisition. UDP delivery is best effort: a successful send does not prove host receipt. Acquisition runs separately from transmission.

Run from the repository root with Python 3.8 or newer; no pip packages are needed:

```bash
python3 host/logger.py --output recordings
python3 host/logger.py --output recordings --bind 0.0.0.0 --port 5005 --duration 60
python3 -m unittest discover -s tests -p 'test_*.py'
```

The host must be reachable at the IP configured in the firmware, with UDP 5005 allowed by its firewall. A guest network with device isolation can prevent delivery. Stop with Ctrl+C. Every invocation creates a new `recordings/run-<UTC timestamp>-<unique suffix>/` directory, containing:

- `packets.jsonl`: all validated sample and metadata packets, preserving received values, with `receive_epoch_ns`, `receive_monotonic_ns`, `source_ip`, `source_port`, and the nested `packet` object.
- `samples.csv`: one row per received sample, including duplicates, original acquisition timestamp, all raw/derived values, individual flag columns, sequence status, and packet diagnostics. JSON null becomes an empty CSV cell. Load count/timestamp columns as integers or strings; spreadsheet floating-point conversion can lose precision.
- `metadata.jsonl`: metadata packets in the same envelope as `packets.jsonl`.
- `stats.json`: receive totals and separate sequence accounting for each `(node_id, boot_id)`, replaced once per second and at clean exit. `invalid_packets` counts rejected datagrams.
- `clock-map.json`: the supplied clock map, if one was requested.

Files flush once per second and on clean exit. This is host storage, not persistent storage on the controllers; a finite node queue cannot preserve an unlimited radio outage. Keep the host logger running for the recording.

## Version 1 packets

All packets require integer `v: 1`, `node_id` 1 (H1), 2 (H2), or 3 (CENTER), and a 16-character hexadecimal `boot_id`. A new boot gets a new ID. JSON numbers must be finite; duplicate keys and nesting beyond 16 levels are rejected. Fields below are required unless specifically described as optional. Additional fields are retained in JSONL, allowing metadata to carry the full firmware configuration.

A `type: "samples"` packet contains `samples` (1–32 records; normally two) and `diagnostics`. Every sample has:

| Field | Type and meaning |
|---|---|
| `seq` | Unsigned 64-bit integer, starts at 0 each boot, increments at acquisition |
| `t_us` | Nonnegative signed 64-bit integer, local acquisition time from `esp_timer_get_time()` |
| `raw_count` | Signed 64-bit **decimal string** for valid H1/H2 snapshots; null for a rejected snapshot or CENTER |
| `raw_spi_word` | Integer 0–65535 for CENTER, including parity failures; null for H1/H2. Ignore this value if `spi_io_error` is set. |
| `position14` | Integer 0–16383 for valid CENTER position; otherwise null |
| `angle_rad` | Relative hemisphere angle or software-unwrapped CENTER angle; finite number or null. CENTER is null when unwrap continuity/rate bound is unverified. |
| `absolute_angle_rad` | CENTER raw single-turn absolute angle; finite number or null |
| `calibrated_alpha_rad` | CENTER wrapped single-turn `wrap(sign * (absolute_angle_rad - alpha_zero_rad))`; null for H1/H2 or invalid CENTER read. Physical reference validity remains a separate flag. |
| `velocity_rad_s` | Raw angular velocity; finite number or null |
| `filtered_velocity_rad_s` | Separately filtered velocity; finite number or null |
| `index_event_count` | Unsigned 64-bit index count; 0 when unavailable |
| `index_timestamp_us` | Last index ISR timestamp on the node clock; 0 when unavailable |
| `index_raw_count` | H1/H2 last index snapshot as signed 64-bit decimal string (usable only with `index_valid`); null for CENTER |
| `flags` | Integer bit mask as defined below |

Signed counts are strings on the wire so JSON tools do not silently truncate values above 2^53. Unsupported or invalid readings are explicitly null. Availability and physical calibration are distinct: use the status flags even when a numerical angle is present. The logger validates types/ranges; it does not independently certify sensor health or calibration.

`diagnostics` contains nonnegative integer counters: `acquisition_overruns`, `queue_drops`, `invalid_spi_responses`, `index_queue_drops`, and `radio_failures`. These are cumulative within the boot and repeated per CSV sample in the packet; do not sum repeated values as independent events.

| Bit | CSV flag name | Meaning |
|---|---|---|
| 0 | `ready` | Startup settling delay completed |
| 1 | `sample_valid` | Acquisition result is valid |
| 2 | `ppr_verified` | Configured hemisphere PPR has been verified |
| 3 | `phase_reference_valid` | Configured physical phase reference has been verified |
| 4 | `unwrap_continuous` | Continuous relative/software-unwrapped interpretation is valid |
| 5 | `clock_sync_valid` | Node clock is synchronized; this firmware leaves it false |
| 6 | `velocity_valid` | Velocity interval is valid |
| 7 | `parity_error` | CENTER parity check failed |
| 8 | `continuity_lost` | Count/angle continuity is uncertain |
| 9 | `index_valid` | A usable index observation has been captured |
| 10 | `rate_bound_verified` | Configured maximum rate has been verified |
| 11 | `acquisition_overrun` | Acquisition deadline was missed |
| 12 | `spi_io_error` | SPI transfer failed |

A `type: "metadata"` packet is sent periodically (about every two seconds). It requires `role` matching the node ID, nonempty `firmware_version` and `idf_version` strings, and includes the firmware's configuration fields such as PPR, sign, offsets, rates, and verification settings. The logger retains all those fields without a fixed configuration-column schema. Start logging before the run and confirm metadata from all three nodes is present.

## Loss and ordering

Sequences are tracked independently for every node and boot. `missing_samples` is the number of unobserved sequence numbers from 0 through the highest sequence received. Late arrival recovers a gap and reduces this count; `recovered_samples` and `reordered_samples` count those late unique samples. A previously received sequence increments `duplicate_samples`. CSV `sequence_status` is `in_order`, `recovered`, or `duplicate`; rows remain in arrival order.

Missing samples include acquisition-to-queue loss, network loss, and samples sent before logging began. They are not a measurement of radio loss alone. Final lost packets cannot be counted until a later sequence arrives. Compare firmware diagnostic counters to distinguish causes. Reboots create a separate stream; never integrate through a reboot as though it were uninterrupted motion.

## Clock alignment

`t_us` belongs to a single node boot. Host receive time includes network and scheduling delay and is **not** a synchronized acquisition timestamp. Matching local timestamps or arrival times across nodes does not establish simultaneous motion. Firmware `clock_sync_valid` stays false; no synchronization accuracy has been measured.

The logger offers an optional interface for an independently calibrated offset/drift fit:

```json
{
  "version": 1,
  "mappings": [
    {
      "node_id": 1,
      "boot_id": "0123456789abcdef",
      "calibrated": false,
      "slope_ns_per_us": "1000.0",
      "offset_ns": 0,
      "valid_from_us": 250000,
      "valid_to_us": 1000000
    }
  ]
}
```

This is a format example, not usable calibration. Set `calibrated: true` only after independently estimating and checking the fit. Supply one mapping per node/boot. The optional validity bounds default to all nonnegative signed 64-bit local times. Recalibrate after a reboot. The slope may be a decimal string or JSON number; `offset_ns` must be a JSON integer.

```bash
python3 host/logger.py --output recordings --clock-map measured-clock-map.json
```

For a calibrated mapping within its validity interval, `mapped_epoch_ns = round(slope_ns_per_us * t_us + offset_ns)` and CSV `mapped_clock_sync_valid` is 1. Otherwise mapped time is empty and validity is 0. Decimal arithmetic preserves nanosecond epoch precision. Original timestamps and firmware flags stay intact. The logger does not estimate calibration from Wi-Fi arrivals or claim a timing tolerance; preserve the calibration procedure and uncertainty with your experiment.

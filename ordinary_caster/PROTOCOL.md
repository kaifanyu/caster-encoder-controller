# Ordinary caster acquisition contract

Two XIAO ESP32-C6 boards per `front_left` / `front_right` read separate channels:
`SWIVEL` (node 5) reads AMT222A-V SPI; `WHEEL` (node 6) reads Hall A/B. These
four boards use v4 UDP and CASTLOG2. Ball firmware keeps v2 UDP and CASTLOG1.
Legacy combined `ORDINARY` (node 4, v3) remains supported. Consumers select
the family and layout before validating inventory.

## Identity and timing

Every split-board packet contains `v: 4`, `caster_type: ordinary`, `node_id: 5`
or `6`, `wheel_id`, stable Wi-Fi MAC `device_id`, 16-hex-character `boot_id`,
and `type`. Metadata role is `SWIVEL` or `WHEEL`. A MAC cannot represent multiple
wheel/role assignments, and combined/split boards must not overlap channels.
Track sequence, time, and boots separately by `(wheel_id, device_id, node_id,
boot_id)`. Acquisition is nominally 200 Hz per board.
UDP ports default to 5007 (left) and 5008 (right). Each live datagram carries one
single-channel sample; a 100 Hz transmission task drains two samples per wake.

A samples packet contains `samples` and the existing five diagnostics
(`acquisition_overruns`, `queue_drops`, `invalid_spi_responses`,
`index_queue_drops`, `radio_failures`). `index_queue_drops` is zero/unused here.
Each sample has `seq`, `t_us`, and only the active `swivel` or `wheel` object.
For node 5, `t_us` timestamps the SPI transaction. For node 6, `t_us` equals
`wheel.t_us` and timestamps the PCNT snapshot. Microseconds are boot-local to
that board. Even two boards on the same caster require independent clock maps.
Do not substitute receipt/download time or assert simultaneous acquisition.
Legacy node 4 includes both channels, with SPI time in `t_us` and its earlier
PCNT time in `wheel.t_us` on that combined board's clock.

| Channel | Required fields |
|---|---|
| swivel | `raw_spi_word`, `position12`, `absolute_angle_rad`, `calibrated_angle_rad`, `angle_rad`, `velocity_rad_s`, `filtered_velocity_rad_s`, `flags` |
| wheel | `t_us`, `raw_count`, `angle_rad`, `velocity_rad_s`, `filtered_velocity_rad_s`, `flags` |

`raw_count` is an exact signed-int64 decimal string, never a JSON float.
`position12` is 0..4095; unavailable numeric measurements are null. Raw SPI words
are retained for diagnosis but are unusable on an I/O error. Invalid swivel
samples do not invalidate wheel samples and vice versa.

Raw swivel angle is [0,2pi). Calibrated body angle is [-pi,pi), calculated as
`wrap_signed(sign * (raw_angle - zero_rad))`. `angle_rad` is software-unwrapped
swivel orientation, requiring a verified physical maximum rate and unambiguous
valid-read intervals. Each recording starts a new unwrap segment. After local
recording, live swivel unwrapped angle stays unavailable until reboot so a
boot-only live receiver cannot join different segment origins.

Wheel angle is startup-relative signed count minus its configured count offset,
scaled by measured counts per revolution. Counting continues across recording
segments. Default CPR=128 is unverified. Default wheel velocity is a freshly
computed trailing 50 ms count-window average, not an instantaneous 5 ms estimate.
Its filter/window lag must be retained in experimental interpretation.

## Channel validity flags

Bits 0..12 retain the ball definitions: ready, sample_valid, ppr_verified,
phase_reference_valid, unwrap_continuous, clock_sync_valid, velocity_valid,
parity_error, continuity_lost, index_valid, rate_bound_verified,
acquisition_overrun, spi_io_error. Bit 13 is `format_error` (AMT12 low bits nonzero).
Wheel `ppr_verified` means the configured x4 **CPR** was measured. Wheel physical
phase and index flags remain false. A valid count does not establish sensor
presence or calibrated scale. Hardware PCNT does not report illegal A/B
transitions; metadata explicitly says `invalid_transition_detection: false`.

Metadata contains only the active sensor's configuration object (both for v3).
These retain
resolution/CPR, sign, zero, verification, velocity filtering/window and GPIOs.
Changing a software sign/zero requires rebuilding that wheel's profile.

## Onboard recording

Discovery UDP 5010 and HTTP 8080 retain the same prepare/start/stop/download/ack
workflow. Control response version stays 1; ordinary responses additionally
identify `caster_type: ordinary`, the board's `node_id`, `record_version: 2`, and
`record_bytes: 116`. The split inventory requires nodes 5 and 6 per selected
wheel, four boards for both casters. Old/unread recordings are protected;
explicit discard semantics and recovery are unchanged.

The 4096-byte header starts with eight bytes `CASTLOG2`, followed by little-endian
uint32 version=2, record_bytes=116, JSON length, and JSON CRC32. JSON at offset24
contains `run_id`, the complete metadata `packet`, and `swivel_unwrap_segment`
(nodes 4/5) or `wheel_velocity_segment` (node 6).
The 4096-byte footer/control records use the existing guarded lifecycle format.
Downloads contain header plus complete records (no footer).

| Byte offset | Type | Content |
|---:|---|---|
| 0 | uint64 | sequence |
| 8 | int64 | acquisition microseconds (swivel for 4/5, wheel for 6) |
| 16 | int64 | wheel acquisition microseconds |
| 24 | int64 | wheel raw count |
| 32 | uint16 | raw SPI word |
| 34 | int16 | position12, -1 when invalid |
| 36 | uint32 | swivel flags |
| 40 | uint32 | wheel flags |
| 44 | uint32 | reserved, must be zero |
| 48 | float64 | swivel absolute angle |
| 56 | float64 | swivel calibrated angle |
| 64 | float64 | swivel unwrapped angle |
| 72 | float64 | swivel velocity |
| 80 | float64 | swivel filtered velocity |
| 88 | float64 | wheel relative angle |
| 96 | float64 | wheel velocity |
| 104 | float64 | wheel filtered velocity |
| 112 | uint32 | CRC32 of preceding 112 bytes |

Python struct format: `<QqqqHhIII8dI`. Unavailable binary doubles are NaN, converted
to null on export. The fixed layout preserves legacy compatibility: inactive
sensor slots are zero/-1/NaN with zero flags and are omitted on decoding into
channel objects/rows. Metadata node identity determines which fields are active.
Stored raw values must be masked by each channel's validity.
Capacity is computed from the partition and record size (about 132.4 s at 200 Hz
with the existing layout). Always use the device's reported capacity. Clock maps
are estimated from explicit exchanges per device/boot, not from Wi-Fi arrivals.

The serializer test emits real C++ wire/binary fixtures when
`ORDINARY_CONTRACT_OUTPUT` is set, for companion decoder conformance testing.

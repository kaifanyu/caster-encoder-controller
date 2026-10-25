# Local recording protocol, version 1

Firmware 0.3.0 adds explicit recording sessions to each encoder board. Wi-Fi
stays connected for discovery, clock measurements, start/stop, and download.
Each board samples independently into its own persistent flash log. From prepare
through finalization, sample and periodic metadata UDP transmission is suppressed.
Live UDP resumes after finalization. Booting never starts a recording.

The Pi must discover and verify the expected six physical devices, prepare all
boards, measure their clocks, start all boards, and only then begin robot motion.
After motion, it stops the boards, measures their clocks again, downloads and
verifies every log, durably saves the data, then acknowledges each log. Start
commands are independent network operations; they do not establish simultaneous
sampling. Acquisition timestamps and per-boot clock fits establish alignment.

## Firmware and partition migration

Build and flash all six wheel/role profiles with the existing helpers. The normal
flash operation includes the partition table. An application-only update does not
create the recorder partition. Missing storage is reported as `state: "error"`,
`stop_reason: "missing_log_partition"`; firmware does not silently fall back to RAM.

`partitions.csv` keeps NVS, PHY, and the 1 MiB application at their former offsets,
and allocates `caster_log` at `0x110000`, size `0x2f0000`, in configured 4 MiB flash.
Root CMake migrates only the partition-table selection/file settings of existing
profile sdkconfig files. It does not flash or erase hardware. Do not change the
partition layout while an unread recording is stored.

The log contains a 4096-byte header, up to **30,720 records of 100 bytes**, and a
separate 4096-byte control/footer sector. At the default 200 Hz this is **153.6
seconds** of nominal samples. The Pi must use `capacity_samples` and `sample_hz`
from each board, allow setup/stop margins, and reject planned runs that cannot fit.
Capacity is per board, not shared between boards.

Prepare erases the partition before recording and writes the header and readiness
marker. A 256-record RAM queue feeds a lower-priority writer that groups up to 16
records per write. The queue is finite; it cannot preserve arbitrarily long flash
or acquisition stalls. Capacity exhaustion or queue overflow automatically stops
the run with an explicit reason. There is no wrapping or overwrite of early data.

Internal flash writes can still delay task-based acquisition on ESP32-C6. Moving
writes to a background task and erasing beforehand do not prove a 5 ms deadline.
Inspect sample intervals, sequence gaps, acquisition-overrun flags, and diagnostics
in a hardware experiment. No sampling or clock-accuracy tolerance is claimed by
the native tests.

## Discovery and HTTP

Broadcast this JSON datagram to UDP **5010**, retaining the source socket for
unicast replies:

```json
{"v":1,"type":"caster_recorder_discover","request_id":"probe-123"}
```

A board echoes the request identifier and replies:

```json
{"v":1,"type":"caster_recorder","request_id":"probe-123","wheel_id":"front_left","device_id":"aa:bb:cc:dd:ee:01","node_id":1,"boot_id":"0123456789abcdef","http_port":8080}
```

Use the reply's source IP and HTTP port. Wi-Fi networks that isolate clients or
block broadcasts need explicitly configured device addresses. Identity is the
wheel/role/device tuple; IP addresses alone are not identity. The service has no
authentication or TLS and is intended for the experiment's trusted local network.

HTTP port is **8080**. JSON mutations contain `run_id`, matching
`[A-Za-z0-9][A-Za-z0-9_.-]{0,63}`. Every successful mutation returns the full status.
Treat the returned state/reason as authoritative; HTTP 200 alone does not mean a
complete recording. Bad JSON receives 400; wrong run, state, or CRC receives 409.

| Request | Behavior |
| --- | --- |
| `GET /api/status` | Current identity plus retained recording status. |
| `GET /api/sync` | Current identity, `boot_id`, `rx_us`, and `tx_us`. Timestamps are local microseconds near handler receipt and reply construction. |
| `POST /api/prepare` | `{"run_id":"..."}`. Erases only idle storage or a previously acknowledged recording. Repeating the current run ID returns its existing status without erasing. |
| `POST /api/start` | `{"run_id":"..."}`. Starts a prepared run in this boot. A retry never changes its original start time. A terminal run with that ID is returned as terminal, never restarted. |
| `POST /api/stop` | `{"run_id":"..."}`. Captures the stop boundary, waits for the acquisition barrier and writer drain, then returns final status. Stopping a prepared run cancels it. Terminal retries are safe. Finalization timeout returns 500; retry stop/status. |
| `GET /api/log?run_id=...` | Binary header plus exactly the finalized valid records. Allowed for stopped/error logs with a valid header and no finalization in progress. The response uses HTTP chunked transfer; restart the download from byte zero on failure. |
| `POST /api/ack` | `{"run_id":"...","crc32":123}`. Verifies the finalized record CRC and persistently marks the log reusable. The Pi must first verify the entire file and durably save it. Does not immediately erase the log. Retries are safe. |

Download and command handlers execute serially in the HTTP server. A prepare
cannot replace a log halfway through its download. Discovery is a separate task.
The stop handler allows up to 30 seconds for finalization; use a longer client
request timeout. Prepare may also take seconds due to flash erasure.

## Status and clock meaning

All status responses include:

- `v: 1`, `wheel_id`, `device_id`, `node_id`, `boot_id`: current device identity and
  current boot. `http_port` and `sample_hz` advertise the service and configured rate.
- `state`: `idle`, `prepared`, `recording`, `stopped`, or `error`.
- `run_id`: selected recording ID, or null when no header is available.
- `log_boot_id`: recording's original boot ID, or null. It can differ from the
  current `boot_id` after a reboot; never apply a new boot's clock fit to old data.
- `sample_count`: records already persisted, not queued samples.
- `data_bytes`: `4096 + 100 * sample_count` for a valid header, otherwise zero.
- `crc32`: incremental zlib-compatible CRC32 over the concatenated **entire
  100-byte record bytes only**, excluding the header and footer. Empty records CRC
  is zero. During recording the count/CRC describe only the persisted prefix.
- `capacity_samples`: maximum stored records for this partition.
- `start_us`, `stop_us`: local timestamps for this log's boot, or null when absent.
  A recovered interrupted run's `stop_us` is the last recovered sample timestamp
  if no final stop footer survived; it is not a measured stop-command timestamp.
- `stop_reason`: empty while prepared/recording, `requested` for normal stop,
  `cancelled` for stop before start, `capacity`, `buffer_overflow`, `storage_error`,
  or a specific storage/recovery error. `interrupted` means no valid matching stop
  footer survived. `prepare_interrupted` identifies an abandoned preparation.
- `acknowledged`: durable permission to reuse the storage for a different run ID.
- `finalizing`: stop requested but queue/barrier/footer not yet complete. This
  remains `state: "recording"` while draining an active run.
- `diagnostics`: final boot-cumulative snapshots of `acquisition_overruns`,
  `queue_drops`, `invalid_spi_responses`, `index_queue_drops`, and `radio_failures`.
  In nonterminal states these are the current boot counters. They include events
  before the recording, including preparation, and are not run-specific totals.
- `diagnostics_known`: false after interrupted recovery without a valid final
  footer. Diagnostic zero placeholders in this case mean unknown, not zero faults.

For synchronization, collect repeated request/response measurements before and
after each run against the Pi's monotonic clock, preserving host send/receive times,
board `rx_us`/`tx_us`, and the boot identity. Fit offset and rate per boot and retain
the uncertainty from network delay/asymmetry. Keep both original and mapped times.
A successful network exchange is not proof of a particular synchronization
accuracy. Firmware `clock_sync_valid` remains false; host mapping validity is
separate. If a board rebooted, its new clock cannot calibrate the retained old log.

## Binary download

All fields use little-endian byte order and explicit serialization. There is no
C++ structure padding in the file. Unsigned and signed 64-bit fields must be read
as integers, not converted through floating-point JSON or spreadsheet cells.

The first 24 header bytes have Python format **`<8sIIII`**:

| Offset | Field |
| --- | --- |
| 0 | Eight bytes `CASTLOG1` |
| 8 | Unsigned version, 1 |
| 12 | Unsigned record size, 100 |
| 16 | Unsigned UTF-8 metadata length |
| 20 | Zlib-compatible CRC32 of the metadata bytes only |
| 24 | Metadata JSON, followed by zero padding through offset 4095 |

Metadata is `{"run_id":"...","packet":{...}}`, where `packet` is the existing
version-2 metadata packet with full wheel/device/role/boot identity and calibration
configuration. Additional metadata keys are allowed. Current firmware adds
`center_unwrap_segment` to identify the per-recording segment behavior.

Each record has Python format **`<QqqHhI5dQqqI`**, exactly 100 bytes:

| Offset | Field | Type |
| --- | --- | --- |
| 0 | `seq` | uint64 |
| 8 | `t_us` | int64 |
| 16 | `raw_count` | int64 |
| 24 | `raw_spi_word` | uint16 |
| 26 | `position14` | int16; -1 unavailable |
| 28 | `flags` | uint32; existing protocol status bits |
| 32 | `angle_rad` | binary64 |
| 40 | `absolute_angle_rad` | binary64 |
| 48 | `calibrated_alpha_rad` | binary64 |
| 56 | `velocity_rad_s` | binary64 |
| 64 | `filtered_velocity_rad_s` | binary64 |
| 72 | `index_event_count` | uint64 |
| 80 | `index_timestamp_us` | int64 |
| 88 | `index_raw_count` | int64 |
| 96 | CRC32 of preceding 96 bytes | uint32 |

Unavailable derived values are IEEE NaN. Sensor role and validity flags determine
whether fixed-width raw fields apply; their presence does not make them valid.
The firmware keeps the original boot sample sequence rather than restarting it
at recording start. Samples outside the inclusive `start_us .. stop_us` acquisition
window are not recorded. Gaps within a log remain visible in sequences/timestamps.

Verify header magic/version/size/metadata CRC, packet identity/run ID, every record
CRC, sequence and time ordering, final byte count, and the aggregate record CRC
against finalized status before ACK. The control/footer sector is never downloaded.

## Power loss and retention

The final sector contains separate once-written 128-byte CRC-protected control
blocks: start at sector offset 0 (`CASTBEG1`), stop at 256 (`CASTEND1`), ACK at 512
(`CASTACK1`), and storage-ready at 768 (`CASTRDY1`). CRC is over bytes 0..123 and
stored little-endian at 124. The stop block retains count/aggregate CRC, start/stop,
stop reason, and final diagnostics; the ACK block binds to count and aggregate CRC.

On boot, the recorder validates the header and scans the contiguous valid record
prefix. A partial final write is discarded; records after a corrupt row are not
invented or skipped over. A valid matching stop footer restores its saved status;
otherwise the prefix is retained as `stopped`/`interrupted`. No resumed capture is
started. An unread recovered log blocks preparation of any different run ID.
Reassigning firmware wheel/role or moving flash between devices produces
`identity_mismatch`, preserving the original header for diagnosis and download.

A small NVS `caster_record/prepare` intent is committed before authorized erasure
and cleared only after the new header/readiness marker succeeds, before start can
be accepted. A pending intent at boot proves that the replacement never reached
recording, so firmware returns idle with `prepare_interrupted` and permits a fresh
prepare. It does not erase automatically. An unknown corrupted header without this
proof remains protected in `error`; do not erase it merely to clear the error.

RAM-queued samples at power loss cannot be recovered. CENTER can establish a new
single-run unwrapped segment after prepare; it cannot recover turns across power
loss or ambiguous read gaps. The acquisition task explicitly resets its CENTER
tracker and velocity history before the first accepted sample of each run. H1/H2
hardware counts are not reset. Never treat CENTER's accumulated angle as continuous
between separate recordings; retain absolute phase and validity flags.
After the first local recording segment in a boot, resumed CENTER live UDP marks
unwrapped angle unavailable and continuity lost until reboot, while preserving
wrapped phase and local velocity. This prevents legacy receivers, which identify
streams only by boot rather than run, from silently joining different unwrap
origins. Per-run local logs retain their own correctly flagged unwrapped samples.

## Validation

`tests/test_recorder_storage.cpp` compiles the actual recorder implementation with
deterministic flash, RTOS, and HTTP substitutes. Its Python runner uses the pinned
ESP-IDF cJSON source through `CJSON_SOURCE_DIRECTORY`. Tests exercise the binary
CRC/endianness, handlers, idempotency, stop/acquisition barrier, durable ACK gates,
reboot identity, torn writes, corruption-prefix recovery, interrupted prepare,
capacity, and queue overflow. These tests do not simulate hardware flash timing,
Wi-Fi latency, battery failure timing, or encoder signals. Those require bench
validation before relying on a trajectory's timing accuracy.

# HAMR caster encoders

Two hardware families have separate acquisition/configuration folders:

- [Ball casters](ball_caster/README.md): the existing H1/H2/CENTER boards and
  legacy commands below remain supported, with unchanged v2 recordings.
- [Ordinary casters](ordinary_caster/README.md): **four ESP32-C6 boards total**,
  one `SWIVEL` and one `WHEEL` on each of front-left and front-right. Each reads
  only its own sensor. In the ESP-IDF PowerShell terminal, build with
  `./ordinary_caster/idf.ps1 front_left SWIVEL` (select each wheel/role), or use
  `bash ordinary_caster/idf.sh front_left SWIVEL build` on Linux/WSL.
  See the [v4/storage contract](ordinary_caster/PROTOCOL.md).

Shared `main/` contains scheduling, Wi-Fi, serialization and the guarded local
recorder. Family-specific drivers are in `ball_caster/` and `ordinary_caster/`.
Ordinary live logs can be captured with `python3 -m ordinary_caster.logger
--wheel front_left --duration 60` (from this repository root); the matching Pi
workflow supports local recording, download, analysis and orientation replay.

One ESP-IDF project runs on six **Seeed XIAO ESP32-C6** boards: H1, H2, and CENTER on each of the **front-left** and **front-right** wheels. Each board reads its own encoder and calculates angle and angular velocity. The Pi can command local flash recording over Wi-Fi and download the verified history after stopping. Live UDP telemetry and the standalone Python logger remain available outside local recording.

| Firmware role | Hardware | Measurement |
|---|---|---|
| `H1` | AMT132S-V + SN74LVC244ADW | Hemisphere 1 rotation |
| `H2` | AMT132S-V + SN74LVC244ADW | Hemisphere 2 rotation |
| `CENTER` | AMT252B-V, direct SPI | Common-rod angle |

All six boards use this repository; select both their wheel and role when building. Each node has its own battery and UBEC. No wires connect rotating nodes. The default receiver is your computer on the same Wi-Fi network; an extra ESP32 receiver is not needed.

```text
front_left  H1/H2/CENTER -> Pi UDP 5005 -> /caster_encoder/front_left/...
front_right H1/H2/CENTER -> Pi UDP 5006 -> /caster_encoder/front_right/...
```

## 1. Check the wiring

Use the [handoff](HAMR_Caster_ESP32_Codex_Handoff.md) as the wiring authority. Its connector numbers, IC pin numbers, board D-labels, and GPIO numbers are different numbering systems.

| Node | Encoder signals at the XIAO |
|---|---|
| H1 / H2 | Buffered A → D0/GPIO0, B → D1/GPIO1, Z → D2/GPIO2 |
| CENTER | CS → D3/GPIO21, SCLK → D8/GPIO19, MISO → D9/GPIO20, MOSI → D10/GPIO18 |

For H1/H2, the buffer runs from **3.3 V** and accepts the encoder's 5 V signals. Follow the handoff for its enable pins and unused pins. CENTER uses 3.3 V SPI signaling and a **10 kΩ CS pull-up to 3.3 V**.

Each node's power path is:

```text
2S battery -> UBEC set to 5.0 V -> encoder +5 V
                             -> Schottky anode -> banded cathode -> XIAO VBUS
UBEC ground -> encoder ground + XIAO ground + buffer ground, if fitted
```

Leave the XIAO BAT pads unused. **Disconnect the battery/UBEC before connecting USB to your computer.** USB power alone may not power the encoder through this diode arrangement. For complete acquisition testing, use the single-source bench-power arrangement you have verified against the handoff.

Hold the mechanism still during power-on. Firmware waits at least 250 ms before accepting measurements; it cannot ensure physical stillness.

## 2. Set the configuration

The firmware uses **ESP-IDF v5.4.2**, target `esp32c6`. There are no third-party firmware libraries. The build helper uses an activated installation of that exact SDK, or the Espressif Docker image pinned by version and digest in `scripts/idf.sh`. The first Docker build downloads the SDK/toolchain image.

The helper also activates a native SDK installed at `~/esp/esp-idf-v5.4.2`
automatically for each command. Set `CASTER_IDF_PATH` to use another native
installation directory. This does not activate ESP-IDF in your parent shell.

```bash
cd /home/para/encoder-controller
cp -n config/front_left.example.h config/front_left.local.h
cp -n config/front_right.example.h config/front_right.local.h
```

Edit these two private files; they are ignored by Git:

| Configuration file | Physical boards | Pi UDP destination |
|---|---|---|
| `config/front_left.local.h` | Front-left H1, H2, CENTER | `RECEIVER_IPV4`, default port **5005** |
| `config/front_right.local.h` | Front-right H1, H2, CENTER | `RECEIVER_IPV4`, default port **5006** |

In **both** files set `WIFI_SSID`, `WIFI_PASSWORD`, and `RECEIVER_IPV4` to the Pi's address. Use a **2.4 GHz** network permitting communication between clients, and allow incoming UDP **5005 and 5006** on the Pi. Reserve the Pi's IP in the router so the compiled destination remains valid. `RECEIVER_IPV4` is always the receiver's IP, not an ESP32 address.

For the Ubuntu hotspot at `10.42.0.1` with the Pi at `10.42.0.14`, set
`RECEIVER_IPV4` to `"10.42.0.14"` in both wheel files, `NETWORK_GATEWAY` to
`"10.42.0.1"`, and `NETWORK_NETMASK` to `"255.255.255.0"` for a `/24` hotspot.
Keep left `UDP_PORT 5005`, right `UDP_PORT 5006`. Empty per-board IPv4 strings
use DHCP; gateway/netmask macros are applied only for a configured static IP.

Each file independently configures H1/H2 PPR, H1/H2/CENTER signs, zeros, and maximum-rate assumptions. Keep verification flags false until measured. Shared defaults are in `config/example.h`; override rates or filters **before** its include in the private wheel file. The original `config/local.h` is no longer read. For this workspace its existing Wi-Fi/receiver settings were copied into both private wheel files, and its calibration/rate settings into front-left only. This assumes the original assembly is front-left; verify that physical assignment before flashing. Front-right retains the example calibration and false verification flags. On another checkout, migrate existing settings manually as appropriate.

For each ESP32's **own IP address**, use either:

- DHCP: leave `H1_IPV4`, `H2_IPV4`, and `CENTER_IPV4` empty; optionally reserve a unique address in the router against the board's `device_id` (its Wi-Fi STA MAC).
- Static addressing: set those three macros independently in each wheel file, and set `NETWORK_GATEWAY` / `NETWORK_NETMASK` for the network. For example, left `192.168.1.111/.112/.113` and right `192.168.1.121/.122/.123`. Choose unused addresses outside the DHCP allocation pool. No DNS is required because the receiver is a literal IPv4 address.

Wheel identity is fixed by the build command, and device identity is read from the chip's stable Wi-Fi STA MAC. IP changes do not change either identity. Never redefine `CASTER_WHEEL_ID`, `CASTER_WHEEL_INDEX`, or `CASTER_NODE_ID` in a configuration header.

Wi-Fi and calibration settings are compiled into the firmware; rebuild and flash the affected boards after changing them. Normally the three boards on a wheel share one receiver/port. A special per-role destination can be expressed with `#if CASTER_NODE_ID == 1` / `2` / `3` around `RECEIVER_IPV4` and `UDP_PORT` definitions, but the Pi must then have listeners on each configured destination.

## 3. Build and upload each board

Label the six physical boards with wheel **and** role before flashing. Build directories and SDK configurations are independent: `build/front_left/H1/`, `build/front_left/H2/`, and so on. Use the matching command with one board connected at a time:

```bash
# Disconnect battery/UBEC before attaching USB. Replace the serial path as needed.
./scripts/idf.sh front_left H1 -p /dev/ttyACM0 flash monitor
./scripts/idf.sh front_left H2 -p /dev/ttyACM0 flash monitor
./scripts/idf.sh front_left CENTER -p /dev/ttyACM0 flash monitor

./scripts/idf.sh front_right H1 -p /dev/ttyACM0 flash monitor
./scripts/idf.sh front_right H2 -p /dev/ttyACM0 flash monitor
./scripts/idf.sh front_right CENTER -p /dev/ttyACM0 flash monitor
```

These are **six separate operations**, each on the board named by that line. Exit each serial monitor with **Ctrl+]**, disconnect that board, and connect the next board before its command. `flash` builds first when needed. To build without a connected board, replace `-p /dev/ttyACM0 flash monitor` with `build`.

The startup log prints `wheel`, `role`, `device_id`, `boot_id`, and the Pi UDP destination. Record the six MAC/device IDs in the Pi's `hamr_bringup/config/caster_encoders.yaml` inventory (`expected_device_ids: ["H1_MAC", "H2_MAC", "CENTER_MAC"]` under `/caster_encoder_front_left` or `/caster_encoder_front_right` → `ros__parameters`). This binds each ROS role to the intended physical board. The optional `expected_source_ips` array uses the same H1/H2/CENTER order; set it only when those addresses are reserved or static. Firmware credentials are never printed. ESP32s send UDP; the Pi bridge publishes ROS topics.

On Linux find USB ports with `ls /dev/ttyACM* /dev/ttyUSB*`. If a board will not enter its bootloader, hold **BOOT**, tap **RESET**, release **BOOT**, and retry. Docker USB flashing is intended for Linux; on macOS or Windows use a native ESP-IDF v5.4.2 environment. After disconnecting USB, reconnect the node's normal power and keep it still while starting.

### CENTER bench check over USB

With the encoder powered using your verified bench arrangement, the serial
monitor works without a Wi-Fi connection. CENTER logs `absolute_rad` (raw
single-turn angle), `alpha_rad` and `alpha_deg` (single-turn angle with software
zero/direction applied), `unwrapped_angle_rad` (accumulated turns), and
`velocity_rad_s`. The UDP protocol fields keep their existing names.

Keep the shaft still after reset, then turn slowly through one full revolution
and back. `alpha_deg` should traverse 0–360 degrees, wrap once per turn, and
return close to its starting value. Check that velocity changes sign when you
reverse direction and that `spi_invalid` stays zero. The software zero is not a
verified physical reference until you calibrate it.

With `CENTER_MAX_RPM_VERIFIED false`, `velocity_rad_s` and
`unwrapped_angle_rad` are deliberately `nan` (`null` in UDP). First test wrapped
position, then establish a physical maximum speed before setting this flag true
in the matching wheel file and reflashing CENTER. The maximum is an upper bound,
not a commanded speed. At 600 RPM the interval between valid reads must be
strictly below about 49.988 ms; the default 200 Hz target is 5 ms. A slower bound
must never be chosen merely to hide an acquisition gap. The offline estimator
below can give provisional velocity for a controlled slow bench run.

If `unwrapped_angle_rad` becomes `nan`, a one-time warning reports the first
continuity-loss reason, the interval between valid samples in microseconds,
and the shortest wrapped candidate displacement in encoder bins. The wrapped
candidate cannot recover missed turns. Valid single-turn `alpha` remains usable;
unwrapped continuity stays lost until reset. An unverified speed bound also
leaves velocity unavailable. Retain the startup and warning lines when debugging.
Disconnected Wi-Fi and increasing `radio_failures` are expected in this USB check.

When building in WSL with the board attached to Windows, use
`python3 scripts/flash_windows.py front_left CENTER --port COM6` (substitute its Windows COM
port). This builds with `scripts/idf.sh` and uses Windows PlatformIO's Python and
esptool to flash the images listed in `build/front_left/CENTER/flasher_args.json`. Add
`--dry-run` to validate an existing build and print the command without building,
resetting, or accessing the serial port; add `--skip-build` to flash an existing
build. Custom Windows installations can supply `--windows-python` and
`--esptool-path`. Review the matching `config/<wheel>.local.h` before flashing; the helper does not
perform a full-chip erase.

`scripts/idf.sh` activates ESP-IDF only inside its own process. The generic
`idf.py flash` and `python -m esptool` commands printed by ESP-IDF therefore do
not activate the SDK in your terminal or connect WSL to a Windows COM port.
Use the Windows helper above for this setup. After a successful build, the build
helper prints its command with the detected port when a single Espressif USB
serial device is connected.

## 4. Record measurements

### Commanded local recording

Use the companion `hamr_control` Pi commands to record only when requested:

```bash
ros2 run hamr_bringup caster_recording discover
ros2 run hamr_bringup caster_recording start trial_001 --duration 60
# Run the trajectory after start succeeds on all six boards.
ros2 run hamr_bringup caster_recording stop trial_001
```

`stop` downloads and verifies the logs into
`$HAMR_BAG_ROOT/encoder_trial_001/` (the existing Pi rosbag root by default),
then acknowledges each durable download. `fetch trial_001` retries transfer.
`--duration` is a capacity check, not an automatic stop. Firmware must be updated
with the new partition table as well as the application on all selected boards.
Wi-Fi remains connected for commands; live sample transmission is suppressed
during preparation and recording. Acquisition timestamps and pre/post clock
exchanges are preserved for offline alignment. At the default 200 Hz, the full
record format holds 153.6 seconds on the current 4 MiB partition layout.

See the [operator workflow](../hamr_control/docs/LOCAL_CASTER_RECORDING.md) and
[storage/control protocol](docs/local-recording-protocol.md) for setup, recovery,
capacity limits, and acquisition qualification. Normal start protects
unacknowledged recordings. Firmware 0.3.1 and the updated Pi client also support
an explicit fresh start, including recovery from `invalid_header`:

```bash
ros2 run hamr_bringup caster_recording --wheel front_left \
  start trial_002 --discard-existing --duration 60
```

`--discard-existing` stops any old active run and deletes the selected boards'
onboard logs without downloading them, then prepares and starts the new run.
Use it on each start when previous onboard data is disposable. Saved host files
remain intact, and a new run ID is required. All selected boards must first be
flashed with firmware advertising `supports_discard_existing:true`; a client-only
update cannot enable this on old firmware. Boot/reconnect never erases storage
or starts a new recording.

### Live UDP recording

For normal Pi operation use the companion `hamr_control` bridge and recorder. Its wheel configuration must match these UDP ports and the six device IDs. ROS topics are under `/caster_encoder/front_left/` and `/caster_encoder/front_right/`; each has `packets`, `metadata`, `diagnostics`, and `H1`, `H2`, `CENTER` `joint_states` topics. The rosbag records both branches separately in one bag.

For a standalone non-ROS bench recording, use Python 3.8 or newer with no pip packages. Start these in two terminals:

```bash
python3 host/logger.py --port 5005 --wheel front_left --output recordings/front_left
python3 host/logger.py --port 5006 --wheel front_right --output recordings/front_right
```

For a Windows computer running WSL, start
`python3 scripts/log_windows.py --wheel front_left --duration 60` from WSL to receive directly in
Windows and save under `recordings/wifi/`. Set firmware `RECEIVER_IPV4` to the
computer's **Windows LAN IPv4 address** on the ESP32's Wi-Fi network; the WSL
address is not the receiver address for this helper. Allow Python to receive
incoming **UDP 5005** on the appropriate Windows Firewall network when prompted.
For the Windows hotspot at `192.168.137.1`, an Administrator PowerShell window
can run `scripts/allow_hotspot_udp.ps1`. It permits only this Windows Python
executable to receive UDP 5005 at that address from `192.168.137.0/24`; address,
subnet, and port are script parameters if the hotspot configuration changes.
Use `--wheel front_right` for port 5006 and allow that port in the firewall too.
`--port`, `--output`, and `--duration` override the defaults; omit `--duration`
to record until Ctrl+C. `--dry-run` prints the command without opening a socket
or creating recordings. Connect the ESP32 to 2.4 GHz Wi-Fi and keep Windows
reachable on the same LAN without client isolation.

To record ROS topics on this WSL laptop instead of a standalone CSV run, use
`python3 scripts/record_rosbag.py --wheel front_left --duration 60`. This optional
WSL helper starts one wheel bridge; use `--wheel front_right` for the other wheel.
For the Pi's normal simultaneous two-wheel recording, use the companion
`caster_encoders.launch.xml` described above. The helper uses the sibling
`hamr_control` checkout's existing bridge, bag recorder, topic filter, and QoS
settings, with the Windows UDP relay in front. It prints a new bag directory
under `recordings/rosbags/` and selects MCAP when installed, otherwise SQLite.
Stop the standalone Windows logger first because the relay needs the same
Windows UDP port. The ROS packet topic retains the ESP acquisition timestamps;
its source IP and receipt time describe the Windows relay hop into WSL. The
selected wheel's MAC inventory is loaded from `caster_encoders.yaml` (override
with `--config`). Optional source-IP expectations must match the relay hop for
this setup, rather than the ESP32's original source address.

For CENTER velocity checks, pass a completed run's `samples.csv` to
`python3 host/center_velocity.py PATH/TO/samples.csv --wheel front_left`. It writes `center_velocity.csv` beside the
recording, using wrapped `position14` differences and ESP acquisition timestamps
instead of Wi-Fi arrival times. These are bench estimates with a provisional
direction sign; no mechanical zero is needed. Reboots, invalid reads, duplicate
or reordered samples, exact half-turn changes, and gaps above 25 ms are handled
explicitly. Each estimate still assumes less than half a turn between accepted
samples; the gap guard does not establish a physical speed bound. Firmware
validity flags and the original recording are preserved.

History is separated by wheel, MAC, and boot; identity conflicts are rejected.
Use `--wheel front_right` for a right run. `--sign -1` reverses the provisional
direction; analyze wheels separately if their physical signs differ, and choose
distinct `--output` paths when selecting both wheels from a combined CSV.

Only one receiver may bind each UDP port: stop the Pi ROS bridges before using the standalone logger on the same Pi. A `--wheel` filter accepts only matching v2 packets, so a misconfigured right wheel cannot be written into a left run. Without `--wheel`, the logger also accepts legacy v1 recordings/traffic and marks their wheel as `legacy`.

Move each labeled encoder slowly and confirm its wheel and role. Stop with **Ctrl+C**, or add `--duration 60`. Each invocation creates a new `run-.../` directory:

- `samples.csv`: every row includes `wheel_id`, `device_id`, `node_id`, `boot_id`, and computer receive times.
- `packets.jsonl` and `metadata.jsonl`: validated packets and configuration with the complete identity.
- `stats.json`: independent per-wheel/device/role/boot sequence accounting, accepted device assignments, and conflict/filter counts.

The standalone logger locks each wheel/role to its first observed device for that run, and rejects a second device claiming that slot or the same device changing slots. A device reboot creates a separate boot stream. The Pi's explicit device inventory additionally checks assignment from the first packet; always fill it for a physical experiment.

Recordings are ignored by Git. See [the protocol notes](docs/protocol.md) for fields, flags, and measured clock mappings. Live UDP uses a bounded in-memory queue and cannot backfill lost packets. Commanded local recording instead retains its flash log through network outages and transfers it after stopping. Sequence and diagnostic counters expose acquisition losses; UDP itself provides no delivery guarantee.

## 5. Understand the measurements

H1/H2 use the hardware pulse counter for x4 quadrature decoding. At the assumed 2048 PPR, one revolution is **8192 counts**:

```text
angle = sign × (raw_count − software_zero_count) × 2π / counts_per_revolution
velocity = sign × change_in_count × 2π / (counts_per_revolution × elapsed_seconds)
```

The raw count continues through index pulses and network outages. Z's **rising edge** updates a cumulative event count and the latest event's timestamp/count; it does not reset the count. These are GPIO-interrupt observations, with unmeasured interrupt latency, not hardware-latched edge measurements. Check `index_valid` before using the index count, especially around counter-limit crossings.

Hemisphere angles are relative to the current boot, and `H1_ZERO_COUNT` / `H2_ZERO_COUNT` only shift that relative coordinate. This version always leaves H1/H2 physical-phase validity false; a runtime physical-reference calibration is future work. Static A/B/Z inputs cannot prove an encoder is connected or powered.

CENTER reads a parity-checked 14-bit single-turn position, from 0 through 16383. `absolute_angle_rad` retains the raw single-turn phase; `calibrated_alpha_rad` applies software zero and direction. `angle_rad` is the software-unwrapped angle and is `null` until the configured rate bound and sample spacing make accumulated rotation trustworthy. Invalid SPI replies do not become a valid zero. Rotations made while the node is powered off cannot be recovered.

Keep the validity flags with the values. PPR verification, physical phase reference, unwrap continuity, and clock synchronization are separate facts. Each node's timestamps are microseconds since its own boot. **Matching node timestamps do not mean simultaneous measurements.** Clock synchronization starts invalid; keep the original timestamps for later clock-offset/drift calibration and ROS/Vicon alignment.

Use the [bench checklist](docs/bench-checklist.md) to verify PPR, signs, wrap behavior, power, and loss/restart behavior before an experiment. Hardware acceptance requires your connected boards and mechanical setup. The [validation record](docs/validation.md) distinguishes executed software checks from outstanding hardware checks.

## Development

```bash
./scripts/test.sh
./scripts/test_protocol.sh  # Actual serializer + pinned SDK cJSON; native SDK or Docker.
```

This runs portable C++ encoder-math tests and Python logger/protocol tests using `g++` and Python 3. CI also builds all six wheel/role combinations with ESP-IDF v5.4.2. Firmware sources live in `main/`; configuration examples are in `config/`; receiver code is in `host/`.

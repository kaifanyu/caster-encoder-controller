# HAMR caster encoders

One ESP-IDF project runs on three **Seeed XIAO ESP32-C6** boards. Each board reads its own encoder, calculates angle and angular velocity, and sends timestamped samples over Wi-Fi. A Python logger saves the measurements on your computer as JSONL and CSV.

| Firmware role | Hardware | Measurement |
|---|---|---|
| `H1` | AMT132S-V + SN74LVC244ADW | Hemisphere 1 rotation |
| `H2` | AMT132S-V + SN74LVC244ADW | Hemisphere 2 rotation |
| `CENTER` | AMT252B-V, direct SPI | Common-rod angle |

All three boards use this repository; select their role when building. Each node has its own battery and UBEC. No wires connect rotating nodes. The default receiver is your computer on the same Wi-Fi network; a fourth ESP32 is not needed.

```text
H1 encoder     -> H1 XIAO     ─┐
H2 encoder     -> H2 XIAO      ├─ Wi-Fi / UDP -> computer -> JSONL + CSV
CENTER encoder -> CENTER XIAO ─┘
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
cd ~/caster-encoder-controller
cp config/example.h config/local.h
```

Edit `config/local.h` before building:

- Set `WIFI_SSID`, `WIFI_PASSWORD`, and your computer's address in `RECEIVER_IPV4`. Use a **2.4 GHz** network with a reachable receiver on the same local network and allow incoming UDP port **5005** on the computer.
- Start with the example's **200 Hz sampling** and **100 Hz transmission**, with two samples per datagram.
- Keep `H1_PPR` / `H2_PPR` at the tentative **2048** and their verification flags false until you measure them. Each node has its own sign and software zero settings.
- Set physically justified maximum rotation rates before marking the corresponding `*_MAX_RPM_VERIFIED` flags true. CENTER's unwrapped turns are not trusted by default; its valid single-turn absolute position remains available.

The private configuration is ignored by Git. Wi-Fi settings are compiled into the firmware; rebuild and flash after changing them. Give the receiver a stable address, such as a router DHCP reservation. Wi-Fi client isolation or a guest network can prevent delivery even when all devices are connected.

## 3. Build and upload each board

Build the three roles:

```bash
./scripts/idf.sh H1 build
./scripts/idf.sh H2 build
./scripts/idf.sh CENTER build
```

Each role has its own directory under `build/`. Label the physical boards before flashing them so H1 and H2 stay distinguishable.

With the battery/UBEC disconnected, connect one board over a USB data cable. On Linux, check its port with `ls /dev/ttyACM* /dev/ttyUSB*`, then upload the matching role:

```bash
./scripts/idf.sh H1 -p /dev/ttyACM0 flash monitor
```

Repeat with `H2` and `CENTER`. Substitute your actual device path. Exit the serial monitor with **Ctrl+]**. If a board will not enter its bootloader, hold **BOOT**, tap **RESET**, release **BOOT**, and retry. The helper's Docker USB path is intended for Linux; on macOS or Windows use a native ESP-IDF v5.4.2 environment for USB flashing.

The startup log identifies the role, GPIOs, firmware version, reset reason, and PPR assumption. USB is only for stationary programming/debugging. After disconnecting USB, reconnect that node's normal power and keep it still while starting.

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

If `unwrapped_angle_rad` becomes `nan`, a one-time warning reports the first
continuity-loss reason, the interval between valid samples in microseconds,
and the shortest wrapped candidate displacement in encoder bins. The wrapped
candidate cannot recover missed turns. Valid single-turn `alpha` remains usable;
unwrapped continuity stays lost until reset. An unverified speed bound also
leaves velocity unavailable. Retain the startup and warning lines when debugging.
Disconnected Wi-Fi and increasing `radio_failures` are expected in this USB check.

When building in WSL with the board attached to Windows, use
`python3 scripts/flash_windows.py CENTER --port COM6` (substitute its Windows COM
port). This builds with `scripts/idf.sh` and uses Windows PlatformIO's Python and
esptool to flash the images listed in `build/CENTER/flasher_args.json`. Add
`--dry-run` to validate an existing build and print the command without building,
resetting, or accessing the serial port; add `--skip-build` to flash an existing
build. Custom Windows installations can supply `--windows-python` and
`--esptool-path`. Review `config/local.h` before flashing; the helper does not
perform a full-chip erase.

`scripts/idf.sh` activates ESP-IDF only inside its own process. The generic
`idf.py flash` and `python -m esptool` commands printed by ESP-IDF therefore do
not activate the SDK in your terminal or connect WSL to a Windows COM port.
Use the Windows helper above for this setup. After a successful build, the build
helper prints its command with the detected port when a single Espressif USB
serial device is connected.

## 4. Record measurements

Use Python 3.8 or newer on the receiver computer; the logger needs no pip packages:

```bash
python3 host/logger.py --output recordings
```

For a Windows computer running WSL, start
`python3 scripts/log_windows.py --duration 60` from WSL to receive directly in
Windows and save under `recordings/wifi/`. Set firmware `RECEIVER_IPV4` to the
computer's **Windows LAN IPv4 address** on the ESP32's Wi-Fi network; the WSL
address is not the receiver address for this helper. Allow Python to receive
incoming **UDP 5005** on the appropriate Windows Firewall network when prompted.
For the Windows hotspot at `192.168.137.1`, an Administrator PowerShell window
can run `scripts/allow_hotspot_udp.ps1`. It permits only this Windows Python
executable to receive UDP 5005 at that address from `192.168.137.0/24`; address,
subnet, and port are script parameters if the hotspot configuration changes.
`--port`, `--output`, and `--duration` override the defaults; omit `--duration`
to record until Ctrl+C. `--dry-run` prints the command without opening a socket
or creating recordings. Connect the ESP32 to 2.4 GHz Wi-Fi and keep Windows
reachable on the same LAN without client isolation.

To record ROS topics on this WSL laptop instead of a standalone CSV run, use
`python3 scripts/record_rosbag.py --duration 60`. The helper uses the sibling
`hamr_control` checkout's existing bridge, bag recorder, topic filter, and QoS
settings, with the Windows UDP relay in front. It prints a new bag directory
under `recordings/rosbags/` and selects MCAP when installed, otherwise SQLite.
Stop the standalone Windows logger first because the relay needs the same
Windows UDP port. The ROS packet topic retains the ESP acquisition timestamps;
its source IP and receipt time describe the Windows relay hop into WSL.

For CENTER velocity checks, pass a completed run's `samples.csv` to
`python3 host/center_velocity.py`. It writes `center_velocity.csv` beside the
recording, using wrapped `position14` differences and ESP acquisition timestamps
instead of Wi-Fi arrival times. These are bench estimates with a provisional
direction sign; no mechanical zero is needed. Reboots, invalid reads, duplicate
or reordered samples, exact half-turn changes, and gaps above 25 ms are handled
explicitly. Each estimate still assumes less than half a turn between accepted
samples; the gap guard does not establish a physical speed bound. Firmware
validity flags and the original recording are preserved.

Start the nodes and move each encoder slowly to confirm the corresponding node is reporting. Stop recording with **Ctrl+C**, or add `--duration 60` to record for one minute. Each run creates a new `recordings/run-.../` directory:

- `samples.csv`: decoded samples and computer receive times, convenient for plotting.
- `packets.jsonl` and `metadata.jsonl`: validated packets and session configuration.
- `stats.json`: per-node/per-boot reception, missing-sequence, and duplicate counts.

Recordings are ignored by Git. See [the protocol notes](docs/protocol.md) for fields, flags, and the optional measured clock mapping. Missing-sequence totals can decrease when delayed packets arrive; repeated samples remain identifiable.

Storage is on the receiver computer. The ESP32 keeps a bounded in-memory queue, so measurements can be lost during wireless outages. Acquisition continues, and counters/sequence numbers expose missing data. There is no SD/flash recording or replay of an outage's missing samples. UDP provides no delivery guarantee.

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
```

This runs portable C++ encoder-math tests and Python logger/protocol tests using `g++` and Python 3. CI also builds all three firmware roles with ESP-IDF v5.4.2. Firmware sources live in `main/`; configuration examples are in `config/`; receiver code is in `host/`.
# caster-encoder-controller

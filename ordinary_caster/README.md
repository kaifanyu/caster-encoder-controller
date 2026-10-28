# Ordinary caster: separate swivel and wheel controllers

Each ordinary caster uses **two Seeed XIAO ESP32-C6 boards**, four boards total:

| Physical board | Firmware role | Node ID | Connected sensor | Pi UDP port |
|---|---|---:|---|---:|
| Front-left swivel | `SWIVEL` | 5 | AMT222A-V top swivel encoder | 5007 |
| Front-left wheel | `WHEEL` | 6 | Both DRV5013 Hall A/B sensors | 5007 |
| Front-right swivel | `SWIVEL` | 5 | AMT222A-V top swivel encoder | 5008 |
| Front-right wheel | `WHEEL` | 6 | Both DRV5013 Hall A/B sensors | 5008 |

Left and right use identical wiring for the same role. Each board has its own
MAC, boot identity and acquisition clock. `SWIVEL` initializes only SPI;
`WHEEL` initializes only Hall quadrature counting. These boards measure passive
motion; they do not drive motors.

The ball-caster driver and entrypoint live in `../ball_caster/`. Common Wi-Fi,
acquisition scheduling, and guarded flash recording are in `../main/`.

## Wiring and mechanical reference

Connect the following AMT222A-V signals to the **SWIVEL board only**:

| AMT222A-V connector | Connection |
|---|---|
| 1: supply | Regulated 5 V |
| 2: SCLK | D8 / GPIO19 |
| 3: MOSI | D10 / GPIO18 |
| 4: GND | Local common ground |
| 5: MISO | D9 / GPIO20 |
| 6: CS | D3 / GPIO21, idle high |

The AMT22 connector differs from the ball setup's AMT25 connector. Do not reuse
AMT25 connector numbering. The AMT222A-V is powered from 5 V but accepts 3.3 V
SPI signals and produces a compatible MISO output; no level shifter is needed.
Keep its underside switch in RUN/SPI mode. Firmware uses mode 0 at 500 kHz,
two zero bytes with CS held low, 3 us setup/interbyte/post-clock delays, and
at least 40 us between reads. It checks parity and the two zero low bits before
extracting the 12-bit count. The housing must be fixed to the non-swiveling
mount; the rotor follows the swivel shaft.

Connect both Hall sensors to the **WHEEL board only**. A and B are two phases
of one wheel encoder, so both belong on this same board.

| Hall connection | WHEEL XIAO |
|---|---|
| A output | D0 / GPIO0 |
| B output | D1 / GPIO1 |
| Each output pull-up | Separate 4.7 kohm resistor to 3V3 |
| Both VCC | 3V3 |
| Both ground | GND |

Use a 100 nF ceramic bypass capacitor at each Hall sensor. The DRV5013 is
open-drain: never pull its outputs up to 5 V. Identify the actual package:
TO-92 LPG/LPE pins 1/2/3 are VCC/GND/OUT; SOT-23 DBZ pins 1/2/3 are VCC/OUT/GND.
Verify orientation from the part drawing. The magnet ring rotates with the
wheel; both sensors remain fixed relative to the fork and must produce
quadrature phases. GPIO2 is unused; there is no Z index. Each sensor shares
ground with its own controller. Route wires so swivel motion cannot wind them
up or bias motion.

The theoretical 64 alternating N/S poles give 32 A cycles and **128 x4 counts
per wheel revolution**, not 256. The sensor phase, air gap, magnetic strength,
actual CPR, signs, and wheel radius require physical measurement. Firmware
cannot prove sensor presence or valid magnetic phasing from static A/B levels.

Use a regulated external 5 V supply for standalone operation; XIAO VBUS is not
a boost converter. Leave BAT pads unused under the existing UBEC power design.
Disconnect battery/UBEC before computer USB. USB alone may not power the
encoder in the diode arrangement: verify a deliberate single-source bench
supply. Hold the swivel stationary at power-on for at least 250 ms.

## Configure and flash

Open the **ESP-IDF v5.4.2 PowerShell terminal**. Create the two private headers
once; each header supplies the common settings for that caster's two boards.
Existing private headers are left intact by these commands:

```powershell
cd C:\Users\kyu\Documents\Upenn\caster-encoder-controller
foreach ($caster in 'front_left', 'front_right') {
    $local = "ordinary_caster/config/$caster.local.h"
    if (!(Test-Path $local)) {
        Copy-Item "ordinary_caster/config/$caster.example.h" $local
    }
}
```

Edit each private header's Wi-Fi SSID/password, `RECEIVER_IPV4` (the Pi), and
calibration. Use a 2.4 GHz network permitting client-to-client traffic. Set
`RECEIVER_IPV4` to the **same Pi address in both files**, obtained from `hostname -I`
on the Pi (choose its Wi-Fi address). For example, use `10.42.0.14` only if that
is the Pi's actual address. Reserve the Pi's address in the router/hotspot.

Leave `SWIVEL_IPV4` and `WHEEL_IPV4` empty for DHCP. If using static addresses,
give all four ESPs distinct unused addresses on the same subnet, with the correct
gateway/netmask. `ORDINARY_IPV4` applies only to legacy combined hardware.
The ESP's address is separate from `RECEIVER_IPV4`. Both left boards send to Pi
UDP 5007; both right boards send to 5008. Role and MAC distinguish the sensors.
Ball ports remain 5005/5006. Recording discovery uses UDP 5010 and commands/downloads
use TCP 8080 on each ESP.

Label the boards, connect one at a time, and find its port with
`[System.IO.Ports.SerialPort]::GetPortNames()`. Substitute the actual port below;
Windows may reuse the same COM number for boards connected one at a time:

```powershell
.\ordinary_caster\idf.ps1 front_left  SWIVEL -Port COM6 -Flash -Monitor
.\ordinary_caster\idf.ps1 front_left  WHEEL  -Port COM7 -Flash -Monitor
.\ordinary_caster\idf.ps1 front_right SWIVEL -Port COM8 -Flash -Monitor
.\ordinary_caster\idf.ps1 front_right WHEEL  -Port COM9 -Flash -Monitor
```

Each command builds that profile, flashes the full image/partition table and
opens the monitor. Exit the monitor with **Ctrl+]** before the next command.
Check the printed wheel, role, MAC and assigned IP and write them on the board's
label. To build only, omit `-Port`, `-Flash`, and `-Monitor`.

For Linux/WSL with ESP-IDF v5.4.2 (or the helper's pinned Docker fallback):

```bash
# One connected, physically labeled board at a time; substitute its serial port.
bash ordinary_caster/idf.sh front_left SWIVEL -p /dev/ttyACM0 flash monitor
bash ordinary_caster/idf.sh front_left WHEEL -p /dev/ttyACM0 flash monitor
bash ordinary_caster/idf.sh front_right SWIVEL -p /dev/ttyACM0 flash monitor
bash ordinary_caster/idf.sh front_right WHEEL -p /dev/ttyACM0 flash monitor

# Build only, with no hardware connected:
bash ordinary_caster/idf.sh front_left SWIVEL build
bash ordinary_caster/idf.sh front_left WHEEL build

# WSL build and Windows USB flash; replace COM6 with the actual port:
python3 scripts/flash_windows.py front_left SWIVEL --port COM6
python3 scripts/flash_windows.py front_left WHEEL --port COM7
```

Linux build products are isolated in `build/ordinary_caster/<wheel>/<ROLE>/`;
the PowerShell helper uses `build/windows/ordinary_caster/<wheel>/<ROLE>/` to
avoid sharing CMake caches across operating systems. A full profile
flash includes the recording partition table. The helper validates wheel and
role before flashing an existing artifact. `--dry-run` on the Windows helper
checks an existing build without building, resetting, or accessing USB.

No private ball credentials or calibration are copied into ordinary headers.
Verification flags default false. Changing a header requires rebuilding and
reflashing that board; a Pi parameter cannot change compiled sensor calibration.

## Calibration and recording interpretation

`SWIVEL_SIGN` and `SWIVEL_ZERO_RAD` turn the AMT count into body-relative swivel
angle in [-pi, pi). Set `SWIVEL_PHASE_REFERENCE_VALID` only after measuring the
physical zero/sign. Raw absolute angle remains in [0, 2pi). Swivel is 12-bit,
4096 positions per turn; it is not the ball carrier's 14-bit angle.

Set `SWIVEL_MAX_RPM` to a measured conservative physical bound and mark
`SWIVEL_MAX_RPM_VERIFIED` only after qualification. Unverified bounds deliberately
leave swivel velocity and accumulated-turn angle unavailable. Software cannot
recover missed turns across an ambiguous interval or power cycle.

Measure several complete wheel turns in both directions before setting
`WHEEL_CPR_VERIFIED`. `WHEEL_ZERO_COUNT` is a relative offset and cannot create
absolute wheel phase at boot. `WHEEL_VELOCITY_WINDOW_US` defaults to 50 ms;
instantaneous 5 ms count differences are coarse at 128 CPR. Preserve this
window in experiment metadata and use comparable analysis bandwidths.

Each board acquires its own channel at 200 Hz by default. Samples include the
sensor's timestamp, validity, angle and velocity. The split schema is version 4,
family `ordinary`, with role/node `SWIVEL`/5 or `WHEEL`/6 and only the active
channel present. The Pi requires all four boards for a two-caster recording.
The same recording/download/analysis workflow is retained, with a separate
clock estimate for every device and boot. Sharing Wi-Fi does not synchronize
the four ESP clocks; the saved timing estimates carry uncertainty.

For old hardware only, explicit role `ORDINARY`/4 retains the combined v3
profile, and old logs remain readable. Select `--ordinary-layout combined` on
the Pi when starting those legacy recordings. Do not use that profile for
the separate swivel/wheel boards.

Run-local recording starts a new swivel unwrap segment and wheel velocity
baseline while keeping cumulative Hall counts. Retain raw data, device/boot/run
identity, per-channel health, and clock estimates. Host receive/download time
is not acquisition time. Physical synchronization accuracy still requires
qualification against the experiment's Vicon/camera timing.

## Bench acceptance

If every read prints `spi_device_acquire_bus: acquire finite time not supported
now`, rebuild and reflash the current SWIVEL firmware. Earlier ordinary firmware
passed a finite bus-lock timeout that ESP-IDF 5.4.2 rejects before transferring
any SPI data. The corrected driver uses `portMAX_DELAY`, matching the SDK's
requirement. `flags=0x1001` means READY plus SPI_IO_ERROR; `swivel_pos=-1` and
`swivel_rad=nan` are invalid measurements, not an angle. This particular error
does not diagnose wiring. After updating, valid raw position should be 0..4095
and wrapped angle finite; velocity can remain unavailable until the physical
maximum rate is verified. Monitor changes in cumulative error counters over
time rather than treating startup totals as current failures.

`flags=0x0081` instead means READY plus PARITY_ERROR: the ESP completed the SPI
transfers, but the received word failed the AMT22 check bits. The serial log
includes `swivel_spi=0x....` to expose that word without accepting it as an angle.
Repeated `0x0000` is consistent with MISO held low, including an unpowered or
disconnected encoder; it cannot identify the exact fault by itself. In the
documented power path (UBEC/encoder 5 V -> diode -> XIAO VBUS), USB cannot power
the encoder backward through the diode while the UBEC is off. Measure the supply
at encoder pins 1 and 4, and verify the SPI-mode switch and connector pin mapping.
Use the established single-source bench-power arrangement; do not bridge the
diode or connect USB and UBEC supplies together as a troubleshooting shortcut.

Even with valid position reads, `SWIVEL_MAX_RPM_VERIFIED=false` deliberately
keeps velocity unavailable. After qualifying a conservative physical rate bound,
set `SWIVEL_MAX_RPM` and mark it verified, then rebuild. Velocity requires two
valid positions with an unambiguous time interval. A verified stationary pair
reports zero rad/s; `nan` means unavailable, not stationary. Physical forward
zero/sign verification (`SWIVEL_PHASE_REFERENCE_VALID`) is a separate calibration.

1. Verify power, every connector pin, Hall package orientation, pull-ups, and
   mechanical wire routing before powering the assembly.
2. Hold still at boot, then turn swivel 90 degrees: expect about 1024 counts;
   cross zero in both directions and confirm parity/format errors remain absent.
3. Turn only the wheel through several marked revolutions. Confirm repeatable
   signed counts, both directions, and the actual counts per revolution.
4. Move each axis separately and together; record all four MACs and ensure
   only the intended caster/channel changes. Reboot one board and confirm its
   companion continues independently and the reboot creates a new identity segment.
5. Verify acquisition intervals during flash recording, network outages,
   stop/download/recovery, known-speed motion, and maximum expected speeds.

Software tests/builds do not establish electrical or mechanical acceptance.
No hardware has been flashed or physically calibrated by adding these profiles.

References: [AMT22 manufacturer datasheet](https://www.mouser.com/datasheet/2/670/amt22_v-1776172.pdf),
[TI DRV5013 datasheet](https://www.ti.com/lit/ds/symlink/drv5013.pdf).

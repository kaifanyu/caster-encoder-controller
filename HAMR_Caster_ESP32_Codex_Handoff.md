# HAMR caster encoder firmware — Codex handoff

Revision: H1 — 2026-10-08

## 1. Task and scope

Implement firmware for **three independent, battery-powered Seeed Studio XIAO ESP32-C6 sensor nodes**. These are passive-caster measurement nodes, not HAMR drive-motor controllers. No motor actuation is requested.

| Logical role | Encoder installed | Measured coordinate | Interface to XIAO |
|---|---|---|---|
| H1 | AMT132S-V | Hemisphere 1 spin, phi_1 | A/B quadrature + Z index, through SN74LVC244ADW |
| H2 | AMT132S-V | Hemisphere 2 spin, phi_2 | Same circuit as H1 |
| CENTER | AMT252B-V | Common-rod / whole-assembly angle, alpha | Direct SPI |

H1, H2, and CENTER each have their own battery, UBEC, XIAO, and local ground. **No power, ground, data, or synchronization wires cross between rotating nodes.** Operational telemetry must be wireless. USB is for stationary bench programming/debugging only.

Use the point-to-point connections below. Earlier generated schematic images had incorrect connections and must not be used as the wiring authority. This document records the agreed design, not a claim that the soldered hardware or firmware has passed bench tests.

The purchased encoders are **not AS5600 or AS5048A**. Do not use those drivers. The hemisphere encoders are incremental, not absolute at power-on. CENTER is the 14-bit **single-turn** AMT252B-V. [S1, S2, S3]

## 2. Numbering conventions and XIAO map

An encoder connector number, a buffer IC pin number, a XIAO D-label, and a GPIO number are different identifiers. Firmware should use the actual GPIO numbers below. [S4, S5]

| XIAO label | GPIO | Usage in this design |
|---|---:|---|
| D0 | 0 | H1/H2: A input |
| D1 | 1 | H1/H2: B input |
| D2 | 2 | H1/H2: Z input |
| D3 | 21 | CENTER: CS output |
| D4 | 22 | Unused; default I2C SDA |
| D5 | 23 | Unused; default I2C SCL |
| D6 | 16 | Unused; default UART TX |
| D7 | 17 | Unused; default UART RX |
| D8 | 19 | CENTER: SPI SCLK output |
| D9 | 20 | CENTER: SPI MISO input |
| D10 | 18 | CENTER: SPI MOSI output |

**SCLK = D8 = GPIO19. I2C SCL = D5 = GPIO23. D3 = GPIO21, NOT GPIO3.**

Suggested central pin-definition header:

```cpp
#pragma once
#include <cstdint>

namespace caster_pins {
// Numeric values are actual ESP32-C6 GPIO numbers, not header positions.
constexpr int ENC_A = 0;
constexpr int ENC_B = 1;
constexpr int ENC_Z = 2;

constexpr int ABS_CS   = 21;
constexpr int ABS_SCLK = 19;
constexpr int ABS_MISO = 20;
constexpr int ABS_MOSI = 18;

// Board-internal RF switch controls; not encoder connections.
constexpr int RF_ENABLE = 3;   // LOW enables RF switch control
constexpr int RF_SELECT = 14;  // LOW selects onboard ceramic antenna
}
```

For the onboard antenna, initialize GPIO3 LOW and GPIO14 LOW using Seeed's RF-switch procedure. Do not confuse internal GPIO3 with the D3 header used for CS. [S4]

Use the existing project's ESP32-C6-compatible framework if present and record its exact version. For Arduino, select XIAO_ESP32C6 with a supported Arduino-ESP32 3.x core. For ESP-IDF, target esp32c6. Use that installed version's peripheral/callback APIs; do not mix incompatible driver generations. [S4, S6]

## 3. H1 and H2 — identical point-to-point wiring

### 3.1 Signal connections

| Signal | AMT132S-V connector | SN74LVC244ADW input | Corresponding buffer output | XIAO input |
|---|---|---|---|---|
| A | 8: A+ | 2: 1A1 | 18: 1Y1 | D0 / GPIO0 |
| B | 10: B+ | 4: 1A2 | 16: 1Y2 | D1 / GPIO1 |
| Z | 12: Z+ | 6: 1A3 | 14: 1Y3 | D2 / GPIO2 |

The buffer input/output pairs are internal: **do not jumper input directly to output**. The buffer is non-inverting. Set the three XIAO signal pins as digital inputs; use no software inversion except an explicit coordinate-sign calibration. External buffers drive them, so pull-ups are not required in normal operation. [S1, S7]

### 3.2 Power, enables, and unused buffer pins

| Component terminal | Connection |
|---|---|
| Encoder 6: +5 V | UBEC regulated +5 V |
| Encoder 4: GND | Local GND |
| Buffer 20: VCC | XIAO 3V3 |
| Buffer 10: GND | Local GND |
| Buffer 1: /1OE | Local GND; used bank always enabled |
| Buffer 19: /2OE | XIAO 3V3; unused bank disabled |
| Buffer unused inputs 8, 11, 13, 15, 17 | Local GND |
| Buffer unused outputs 3, 5, 7, 9, 12 | No external connection |

Buffer power is **3.3 V**, not 5 V; its signal inputs tolerate the encoder's 5 V outputs. Buffer enables are hardwired and consume no MCU pins. [S7]

Encoder pins other than 4, 6, 8, 10, 12 are not wired in this build. In particular, encoder UART/configuration pins are unavailable to firmware; do not assume an ESP can change its hardware PPR through this wiring. Encoder pin 18 is not a replacement for power-ground pin 4. [S1]

### 3.3 Hemisphere acquisition requirements

Use the ESP32-C6 **hardware PCNT peripheral** for x4 quadrature decoding: both edges of A and B, with direction derived from their phase. Do not poll A/B at the sample rate or send a radio packet on every encoder edge. [S6]

Use one PCNT unit and two channels per hemisphere. Configure hardware counter limits, enable the driver's overflow accumulation, and register the required high/low watch points. Implement safe extension into an application int64_t count; do not assume the driver's accumulated `int` can never overflow. Obtain atomic/consistent snapshots. Keep the counter running rather than stop/read/clear it every sample. [S6]

The AMT13 datasheet lists **2048 PPR as its factory default**, with x4 decoding giving 8192 counts/revolution. **The actual setting of these purchased encoders is not yet measured or confirmed.** Expose PPR per node and report whether it has been verified. A is documented to lead B for CCW rotation viewed from the manufacturer's front; installation may reverse the sign relevant to our mechanism. [S1]

Proposed initial configuration, not a measured result:

```text
encoder_ppr = 2048
quadrature_multiplier = 4
counts_per_rev = encoder_ppr * quadrature_multiplier
ppr_verified = false
coordinate_sign = +1                 # independently calibrate H1 and H2
phase_reference_valid = false
```

Proposed processing:

```text
relative_angle_rad = sign * (count - software_zero_count) * 2*pi / counts_per_rev
angular_velocity_rad_s = sign * (count_now - count_prev) * 2*pi
                         / (counts_per_rev * actual_dt_seconds)
```

Expose raw count even when PPR/reference is unverified. Static A/B/Z levels do not prove an incremental encoder is powered or connected; do not claim automatic sensor-presence detection from those levels alone. Do not silently present startup-relative zero as known physical shell phase. Keep counts continuous across network outages; invalidate continuity after MCU restart.

Capture Z with a short GPIO interrupt, recording an event count and local timestamp; use an ISR-safe PCNT snapshot method where supported. Do not poll Z at 200 Hz. Define which Z edge is recorded and document ISR latency. The index is narrow and occurs once per revolution. [S1, S6]

Do **not** reset the accumulated count at every Z event. Keep index-based phase calibration separate from raw count. For initial PPR testing, rotate monotonically through multiple complete turns and compare successive same-edge index counts. Direction reversal near the index is not a reliable one-revolution test; direction also affects which edge of the finite-width index pulse is encountered first.

Glitch-filter width must be configurable and checked against actual PPR/RPM. Do not apply millisecond-scale mechanical-button debounce to these electronic encoder outputs. Preserve raw samples and make any velocity filtering separately configurable.

## 4. CENTER — direct SPI wiring

| AMT252B-V connector | Connection |
|---|---|
| 1, 2 | Not connected; configuration only |
| 3: CS | XIAO D3 / GPIO21 |
| 4: SCLK | XIAO D8 / GPIO19 |
| 5: MOSI | XIAO D10 / GPIO18 |
| 6: MISO | XIAO D9 / GPIO20 |
| 7: GND | Local GND |
| 8: +5 V | UBEC regulated +5 V |

No level shifter is used. AMT252B-V SPI has 3.3 V signaling compatibility despite its 5 V supply. A 10 kOhm CS-to-3V3 pull-up is part of the recommended circuit; verify that it was installed. Never pull CS up to 5 V. [S2]

### 4.1 SPI driver requirements

AMT25 uses **Mode 0, MSB first, two 8-bit transfers**, maximum 2 MHz. Read position by transmitting **0x00, 0x00** while holding CS LOW across both bytes. Observe minimum setup/interbyte/post-clock delays of **2.5/2.5/3 microseconds**, and at least **40 microseconds with CS HIGH between reads**. [S2]

Initialize CS as an output HIGH before the first transaction. Ensure SPI driver auto-CS does not toggle CS between the two bytes.

Proposed conservative bring-up transaction:

```text
SPI clock = 500000 Hz, Mode 0, MSB first
CS LOW
wait 3 us
high_byte = transfer8(0x00)
wait 3 us
low_byte = transfer8(0x00)
wait 3 us
CS HIGH
wait at least 40 us before another transaction, including any retry
word = (high_byte << 8) | low_byte
```

Do not replace this with a continuous transfer16() that eliminates the interbyte gap. For Arduino the pin initialization is:

```cpp
// Order: SCK, MISO, MOSI, SS
SPI.begin(19, 20, 18, 21);
```

Validate the two upper parity bits before extracting the 14-bit position. Invalid responses must set an error flag, not become angle zero. [S2]

Parity expressed using bit 0 = least-significant bit:

```text
bit15 = NOT XOR(bits13,11,9,7,5,3,1)
bit14 = NOT XOR(bits12,10,8,6,4,2,0)
position14 = word & 0x3FFF
```

This parity form is also illustrated in the manufacturer's related AMT22 sample; use the **AMT25** timing and connector assignments, not the AMT22 pinout. [S8]

Suggested host unit tests: response 0x61AB must validate and decode to 8619; 0xC000 must validate and decode to zero; 0x0000 and 0xFFFF must fail; every single-bit flip of a valid word must fail. The latter tests follow directly from the stated parity equations.

### 4.2 Center angle processing

Use 16384 bins, not 4096. Do not apply the right shift used for a 12-bit variant. This part supplies one-turn position; it does not supply a hardware turn count. [S2, S3]

Implement:

```text
absolute_angle_rad = position14 * 2*pi / 16384
calibrated_alpha = wrap(sign * (absolute_angle_rad - alpha_zero_rad))
```

Maintain software-unwrapped angle only across sufficiently frequent valid samples. Use modular differences in (-8192, 8192); a half-turn difference is ambiguous. On long gaps, invalid samples, or reset, mark unwrapped continuity uncertain when a configured maximum angular rate cannot rule out multiple turns. Never infer rotations that happened while power was off. Preserve absolute raw phase separately from software zeroing.

Do not automatically send encoder reset or nonvolatile zero commands. Provide a software zero offset first; any encoder-write command must require explicit user action.

## 5. Shared power and boot assumptions

Agreed power topology per node:

```text
Own 2S battery -> own UBEC set to 5.0 V
UBEC OUT+ -> encoder +5 V
UBEC OUT+ -> Schottky anode; banded cathode -> XIAO VBUS
UBEC OUT- -> all local GND connections
XIAO 3V3 -> buffer supply/enables on hemispheres only
```

Leave BAT pads unused; raw 2S voltage must not reach VBUS, BAT, 3V3, or GPIO. Do not assume the external diode safely isolates a connected PC: disconnect battery/UBEC before USB programming. Standalone USB firmware testing does not guarantee the encoder has power through the upstream diode arrangement. Test complete acquisition using a deliberately verified, single-source bench-power configuration. [S4, S9]

Both encoder families specify about 200 ms stationary startup. **Require the operator to hold the mechanism still at power-on; a firmware delay cannot enforce mechanical stillness.** Use at least 250 ms readiness delay after power stabilizes before accepting measurements. [S1, S2]

Bypass capacitance is hardware-only. Use local ceramic bypass at the buffer supply; a compact 1 uF ceramic was accepted for the prototype, while TI recommends 0.1 uF and optionally additional parallel capacitance. Its grounded leg can attach at any solder joint already wired to the local GND net, including grounded pin 15, provided the return path is short. No GPIO controls the capacitor. [S7]

No battery-voltage divider or switched-power GPIO is currently specified. Do not invent battery ADC readings, a low-voltage cutoff, or an encoder power-control pin. D0/GPIO0 is already encoder A on H1/H2.

## 6. Sampling, transport, and timestamps

The following are **proposed initial firmware settings**, not measured performance or previously finalized choices:

```text
Node IDs: H1=1, H2=2, CENTER=3
Acquisition snapshot/read rate: 200 Hz per node
Wireless transmit rate: 100 Hz per node, batching two timestamped samples
Status/debug print rate: 5 Hz or lower
Transport: preserve an existing project transport; otherwise make it configurable
```

Buffer acquisition separately from telemetry. A transmission delay/drop must not stop counting or block sampling. Keep interrupt/radio callbacks short and send work through queues. Count acquisition overruns, queue drops, invalid SPI responses, and radio failures separately. Do not enable sleep modes that pause acquisition during an experiment. [S6, S10]

Use int64_t microsecond timestamps from esp_timer_get_time() at acquisition, not packet-send time. The timer is local to each boot. Add a boot/session identifier and monotonically increasing sample sequence. Do not wait indefinitely for a USB Serial connection before starting a battery-powered node. [S11]

The wireless protocol and stationary receiver have **not** been confirmed. ESP-NOW with a stationary ESP32 USB/UART gateway was proposed; Wi-Fi/UDP to a host is another possible transport. Do not assume a fourth ESP already exists or require wiring from CENTER to the host.

For ESP-NOW, configure matching radio channel/peer addresses and the correct callbacks for the installed SDK. Send success is not proof of application-level receipt. Sequence numbers and host-side loss accounting are required. [S10]

Suggested sample record (explicitly serialize; do not rely on compiler struct padding):

```text
protocol_version
node_id / encoder_type
boot_id
sample_sequence
sample_timestamp_us
raw_count_int64             # H1/H2 only
raw_spi_word / position14   # CENTER only
relative_or_unwrapped_angle_rad
angular_velocity_rad_s
index_event_count           # H1/H2 only
status_flags
```

Define flags for sensor readiness, sample validity, PPR verification, phase-reference validity, unwrap continuity, and clock-sync validity. Include sign, PPR, zero offsets, firmware version, and rates in session metadata. Record receiver timestamps separately. A repeated last-good value must be explicitly marked stale.

**The three local clocks are not automatically synchronized.** Do not join samples from different nodes merely because their local timestamps match or packets arrived together. Implement/leave a clean interface for clock-offset/drift estimation against the receiver, retain original timestamps, and mark sync invalid until calibrated. This is necessary for later ROS/Vicon/camera alignment. Do not promise synchronization precision until measured.

## 7. Deliverables and acceptance checks

Please implement one configurable project with H1, H2, and CENTER build/runtime roles. Separate encoder acquisition, calibration, packet serialization, and transport. Include build/flash instructions, pinned dependency versions, an example configuration, and a host logger or decoder with unit tests.

Bench acceptance requirements:

1. Print role, actual GPIO assignments, PPR assumption, firmware version, and reset reason at startup. Boot without a USB host attached.
2. Verify H1/H2 count smoothly in both directions; measure counts over several complete monotonic revolutions. At 2048 PPR, expect 8192 x4 counts per revolution. Check high/low counter-limit crossings without jumps.
3. Record Z events without resetting raw count. Calibrate physical sign independently for the two hemispheres; do not assume their mounting directions match.
4. Verify CENTER parity, raw range, direction, and continuous wrap through 16383 -> 0 and 0 -> 16383. An unplugged/invalid sensor must not appear as a valid zero angle.
5. Verify continued acquisition during wireless loss, queue saturation, and reconnect; distinguish sensor errors from transport loss.
6. Restart one node and verify boot_id/reset flags prevent false continuous integration or false clock synchronization.
7. Compare the wiring/power measurements with this handoff before powering attached encoders. Report which checks were actually executed versus only compiled or unit-tested.

## 8. Open items — keep explicit rather than guessing

Actual AMT132S PPR settings; final mechanical signs/zero poses; maximum expected RPM; selected wireless transport; receiver hardware/address/channel; required synchronization tolerance; exact framework/repository. These do not prevent implementing the fixed pin map, the two drivers, diagnostics, and unit tests. Keep all uncertain settings configurable and visible in metadata.

## Source references

Manufacturer datasheets hosted by distributors remain manufacturer-authored sources. Page numbers below are the documents' printed page numbers.

- [S1] CUI Devices / Same Sky AMT13 datasheet, pages 1-2 (resolution/startup/index), 4 (AMT132S connector). https://www.mouser.com/datasheet/2/670/amt13_v-3006045.pdf
- [S2] Same Sky AMT25 datasheet, pages 1-3 (electrical/SPI), 5 (connector). https://www.mouser.lt/datasheet/2/1628/amt25_v-3569567.pdf
- [S3] Same Sky exact AMT252B-V product identification. https://www.sameskydevices.com/product/motion-and-control/rotary-encoders/absolute/modular/amt252b-v
- [S4] Seeed XIAO ESP32-C6 official pin map, RF switch, board setup, and power notes. https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
- [S5] Espressif Arduino-ESP32 XIAO_ESP32C6 board definitions. https://raw.githubusercontent.com/espressif/arduino-esp32/master/variants/XIAO_ESP32C6/pins_arduino.h
- [S6] Espressif ESP32-C6 PCNT documentation. Match the documentation version to the installed SDK. https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/peripherals/pcnt.html
- [S7] TI SN74LVC244A datasheet; use the DW/SOIC-20 pinout, not the different RWP package. https://www.ti.com/lit/ds/symlink/sn74lvc244a.pdf
- [S8] Same Sky related AMT22 SPI/parity sample; **not** the authority for AMT25 pinout. https://www2.sameskydevices.com/product/resource/sample-code/amt22
- [S9] Seeed external-power notes. https://wiki.seeedstudio.com/xiao_pin_multiplexing_esp32c6/
- [S10] Espressif ESP32-C6 ESP-NOW documentation. https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/network/esp_now.html
- [S11] Espressif ESP32-C6 high-resolution timer documentation. https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/esp_timer.html

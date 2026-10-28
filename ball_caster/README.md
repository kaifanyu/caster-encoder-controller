# Ball caster controller

The ball caster retains three independent XIAO ESP32-C6 controllers per wheel:
H1 and H2 read AMT132S-V incremental hemisphere encoders; CENTER reads the
AMT252B-V 14-bit common-rod/carrier encoder. Front-left and front-right therefore
use six boards. A ball carrier angle is not an ordinary caster swivel angle.

The hardware driver is `encoders.cpp` in this folder. Shared scheduling,
transport, and flash recording remain in `../main/`. The ordinary caster has
its own driver, configuration and instructions in `../ordinary_caster/`.

New private ball settings live in `config/front_left.local.h` and
`config/front_right.local.h` in this folder. Existing settings in the root
`../config/` remain supported as a fallback; no migration or overwrite is
necessary. If both locations contain a private header for the same wheel,
the helper and CMake refuse to choose between them. Ordinary profiles cannot
read these ball headers.

From the repository root:

```bash
# For a NEW installation only; keep existing root private headers if present.
cp -n ball_caster/config/front_left.example.h ball_caster/config/front_left.local.h
cp -n ball_caster/config/front_right.example.h ball_caster/config/front_right.local.h

bash ball_caster/idf.sh front_left H1 build
bash ball_caster/idf.sh front_left H2 build
bash ball_caster/idf.sh front_left CENTER build
# Repeat the desired role with front_right for the other wheel.
# For a connected board, substitute its actual port and matching role:
bash ball_caster/idf.sh front_left CENTER -p /dev/ttyACM0 flash monitor
```

These commands retain the legacy `build/<wheel>/<role>/` artifacts, telemetry
version 2, UDP 5005/5006, and three-device-per-wheel recording inventory. See
the [root setup guide](../README.md), [wiring handoff](../HAMR_Caster_ESP32_Codex_Handoff.md),
and [bench checklist](../docs/bench-checklist.md) for electrical and calibration
requirements. Disconnect battery/UBEC before computer USB and keep the mechanism
stationary at power-on. These measurement boards do not command motors.

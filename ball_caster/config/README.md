# Ball caster calibration compatibility

New ball profiles use `ball_caster/config/front_left.local.h` and
`ball_caster/config/front_right.local.h`. Existing installations can keep
`config/front_left.local.h` and `config/front_right.local.h` at the repository
root instead. This legacy fallback preserves deployed Wi-Fi settings and
measured calibration. Neither location reads `ordinary_caster/config`.

The example headers here mirror the root examples. For a new installation,
copy the wheel example to its matching `.local.h` in this folder. Do not copy
an example over an existing calibration. If both canonical and legacy private
headers exist for a wheel, the helper and CMake reject the ambiguity rather
than choosing one silently. Keep exactly one authoritative header per ball wheel.

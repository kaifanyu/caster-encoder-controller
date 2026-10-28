#pragma once
// Shared defaults for separate AMT222A-V SWIVEL and Hall A/B WHEEL boards.
// These are starting assumptions, not measured hardware calibration.
#define ORDINARY_CASTER_CONFIG 1
#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "CHANGE_ME"
#endif
#ifndef RECEIVER_IPV4
#define RECEIVER_IPV4 "192.168.1.100"
#endif
#ifndef UDP_PORT
#define UDP_PORT 5007
#endif
#ifndef ORDINARY_IPV4
#define ORDINARY_IPV4 ""
#endif
// Separate physical boards must never inherit a single static address.
#ifndef SWIVEL_IPV4
#define SWIVEL_IPV4 ""
#endif
#ifndef WHEEL_IPV4
#define WHEEL_IPV4 ""
#endif
#ifndef NETWORK_GATEWAY
#define NETWORK_GATEWAY "192.168.1.1"
#endif
#ifndef NETWORK_NETMASK
#define NETWORK_NETMASK "255.255.255.0"
#endif
#ifndef SAMPLE_HZ
#define SAMPLE_HZ 200
#endif
#ifndef TRANSMIT_HZ
#define TRANSMIT_HZ 100
#endif
#ifndef SWIVEL_SIGN
#define SWIVEL_SIGN 1
#endif
#ifndef SWIVEL_ZERO_RAD
#define SWIVEL_ZERO_RAD 0.0
#endif
#ifndef SWIVEL_PHASE_REFERENCE_VALID
#define SWIVEL_PHASE_REFERENCE_VALID false
#endif
#ifndef SWIVEL_MAX_RPM
#define SWIVEL_MAX_RPM 600.0
#endif
#ifndef SWIVEL_MAX_RPM_VERIFIED
#define SWIVEL_MAX_RPM_VERIFIED false
#endif
// 64 alternating magnetic poles imply 32 A cycles and 128 x4 counts/turn.
// Measure actual CPR and signs before marking this scale verified.
#ifndef WHEEL_CPR
#define WHEEL_CPR 128
#endif
#ifndef WHEEL_CPR_VERIFIED
#define WHEEL_CPR_VERIFIED false
#endif
#ifndef WHEEL_SIGN
#define WHEEL_SIGN 1
#endif
#ifndef WHEEL_ZERO_COUNT
#define WHEEL_ZERO_COUNT 0LL
#endif
// Windowed rate avoids a one-count / 5 ms derivative at this coarse resolution.
#ifndef WHEEL_VELOCITY_WINDOW_US
#define WHEEL_VELOCITY_WINDOW_US 50000LL
#endif
#ifndef PCNT_GLITCH_FILTER_NS
#define PCNT_GLITCH_FILTER_NS 0
#endif
#ifndef VELOCITY_FILTER_TAU_S
#define VELOCITY_FILTER_TAU_S 0.0
#endif

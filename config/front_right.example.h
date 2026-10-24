#pragma once
// Copy to front_right.local.h. This file configures only the front_right wheel.
// Identity comes from the selected build profile; never set CASTER_WHEEL_ID here.
#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"
#define RECEIVER_IPV4 "192.168.1.100"  // The Pi, not this ESP32.
#define UDP_PORT 5006

// Optional fixed addresses per physical board. Empty means DHCP (recommended:
// reserve addresses in the router using each board's startup device_id/MAC).
#define H1_IPV4 ""      // Example: 192.168.1.121
#define H2_IPV4 ""      // Example: 192.168.1.122
#define CENTER_IPV4 ""  // Example: 192.168.1.123
#define NETWORK_GATEWAY "192.168.1.1"
#define NETWORK_NETMASK "255.255.255.0"

// Independent calibration for this wheel. Verify these on the physical hardware.
#define H1_PPR 2048
#define H2_PPR 2048
#define H1_PPR_VERIFIED false
#define H2_PPR_VERIFIED false
#define H1_SIGN 1
#define H2_SIGN 1
#define CENTER_SIGN 1
#define H1_ZERO_COUNT 0LL
#define H2_ZERO_COUNT 0LL
#define CENTER_ZERO_RAD 0.0
#define CENTER_PHASE_REFERENCE_VALID false
#define H1_MAX_RPM 600.0
#define H2_MAX_RPM 600.0
#define CENTER_MAX_RPM 600.0
#define H1_MAX_RPM_VERIFIED false
#define H2_MAX_RPM_VERIFIED false
#define CENTER_MAX_RPM_VERIFIED false

// Override additional defaults here (before including example.h), e.g.
// SAMPLE_HZ, TRANSMIT_HZ, PCNT_GLITCH_FILTER_NS, VELOCITY_FILTER_TAU_S.
// Per-role destinations are possible with #if CASTER_NODE_ID == 1 / 2 / 3,
// but all three boards normally publish to this wheel's one Pi UDP listener.
#include "example.h"

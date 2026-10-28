#pragma once
// Copy to front_right.local.h; same pin map as front-left, independent calibration.
// Shared calibration/network settings for this caster's separate SWIVEL and WHEEL boards.
#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"
#define RECEIVER_IPV4 "192.168.1.100" // Pi/receiver address, not this board.
#define UDP_PORT 5008
#define SWIVEL_IPV4 "" // DHCP; or a unique reserved address for the swivel board.
#define WHEEL_IPV4 "" // DHCP; or a DIFFERENT reserved address for the wheel board.
#define ORDINARY_IPV4 "" // Legacy combined-board profile only.
#define NETWORK_GATEWAY "192.168.1.1"
#define NETWORK_NETMASK "255.255.255.0"
#define SWIVEL_SIGN 1
#define SWIVEL_ZERO_RAD 0.0
#define SWIVEL_PHASE_REFERENCE_VALID false
#define SWIVEL_MAX_RPM 600.0 // Provisional physical upper bound; not a command.
#define SWIVEL_MAX_RPM_VERIFIED false
#define WHEEL_CPR 128
#define WHEEL_CPR_VERIFIED false
#define WHEEL_SIGN 1
#define WHEEL_ZERO_COUNT 0LL
#define WHEEL_VELOCITY_WINDOW_US 50000LL
// Override shared rates/filters above this include. Never override CASTER_* IDs.
#include "example.h"

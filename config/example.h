#pragma once

// Copy this file to config/local.h. local.h and build/ are ignored by Git.
// Use a 2.4 GHz network with client-to-client communication enabled.
#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"
#define RECEIVER_IPV4 "192.168.1.100"
#define UDP_PORT 5005

// Two acquisition samples per UDP transmission; default 200 Hz / 100 Hz.
#define SAMPLE_HZ 200
#define TRANSMIT_HZ 100

// Proposed assumptions, NOT measured calibration results.
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
// Hemispheres have no physical reference at boot; firmware always leaves their
// phase-reference flag false. H*_ZERO_COUNT only shifts the relative coordinate.
#define CENTER_PHASE_REFERENCE_VALID false

// Disabled initially. Choose a width below the shortest valid edge spacing
// after PPR and the actual maximum RPM are confirmed; units are nanoseconds.
#define PCNT_GLITCH_FILTER_NS 0
// This bound is used for continuity checks, not as a speed measurement.
// Set VERIFIED true ONLY after establishing an upper bound for your mechanism.
#define H1_MAX_RPM 600.0
#define H2_MAX_RPM 600.0
#define CENTER_MAX_RPM 600.0
#define H1_MAX_RPM_VERIFIED false
#define H2_MAX_RPM_VERIFIED false
#define CENTER_MAX_RPM_VERIFIED false
// Zero disables the optional low-pass filter; raw velocity is always retained.
#define VELOCITY_FILTER_TAU_S 0.0

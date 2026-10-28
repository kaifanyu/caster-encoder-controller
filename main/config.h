#pragma once
#include "encoders.h"
// CMake fixes the selected profile and wheel identity together.
#ifndef CASTER_CONFIG_HEADER
#error "Select front_left or front_right with scripts/idf.sh"
#endif
enum { kSelectedWheelIndex = CASTER_WHEEL_INDEX, kSelectedNodeId = CASTER_NODE_ID };
#include CASTER_CONFIG_HEADER
static_assert(CASTER_WHEEL_INDEX == kSelectedWheelIndex, "Configuration must not change the wheel");
static_assert(CASTER_NODE_ID == kSelectedNodeId, "Configuration must not change the role");
#ifdef CASTER_WHEEL_ID
#error "Do not override CASTER_WHEEL_ID in configuration"
#endif
#if CASTER_WHEEL_INDEX == 1
#define CASTER_WHEEL_ID "front_left"
#elif CASTER_WHEEL_INDEX == 2
#define CASTER_WHEEL_ID "front_right"
#else
#error "Unknown wheel profile"
#endif

static_assert(CASTER_NODE_ID >= 1 && CASTER_NODE_ID <= 6, "Unknown role");
#if CASTER_NODE_ID >= 4
#ifndef ORDINARY_CASTER_CONFIG
#error "Ordinary roles require their own ordinary_caster/config wheel header"
#endif
static_assert(WHEEL_CPR > 0, "WHEEL_CPR must be positive");
static_assert(WHEEL_SIGN == 1 || WHEEL_SIGN == -1, "WHEEL_SIGN must be +1 or -1");
static_assert(SWIVEL_SIGN == 1 || SWIVEL_SIGN == -1, "SWIVEL_SIGN must be +1 or -1");
static_assert(WHEEL_VELOCITY_WINDOW_US >= 1000 && WHEEL_VELOCITY_WINDOW_US <= 1000000,
              "Wheel velocity window must be between 1 ms and 1 s");
static_assert(WHEEL_VELOCITY_WINDOW_US >= 1000000 / SAMPLE_HZ,
              "Wheel velocity window must be at least one acquisition interval");
#else
#ifdef ORDINARY_CASTER_CONFIG
#error "Ball profiles must not use an ordinary-caster configuration"
#endif
#endif
static_assert(SAMPLE_HZ >= 2 && SAMPLE_HZ <= 1000, "Sample rate must be 2..1000 Hz");
static_assert(SAMPLE_HZ == 2 * TRANSMIT_HZ, "Batching requires sample Hz = 2 * transmit Hz");
static_assert(1000000 % SAMPLE_HZ == 0 && 1000 % TRANSMIT_HZ == 0,
              "Rates must divide timer microseconds and scheduler milliseconds");
static_assert(UDP_PORT > 0 && UDP_PORT <= 65535, "Invalid UDP port");

namespace caster {
#if CASTER_NODE_ID >= 4
constexpr const char* kFirmwareVersion = "0.5.0";
constexpr const char* kCasterType = "ordinary";
constexpr const char* kLocalIpv4 = CASTER_NODE_ID == 5 ? SWIVEL_IPV4 :
                                  CASTER_NODE_ID == 6 ? WHEEL_IPV4 : ORDINARY_IPV4;
constexpr const char* kRole = CASTER_NODE_ID == 5 ? "SWIVEL" : CASTER_NODE_ID == 6 ? "WHEEL" : "ORDINARY";
#else
constexpr const char* kFirmwareVersion = "0.3.1";
constexpr const char* kCasterType = "ball";
constexpr const char* kLocalIpv4 = CASTER_NODE_ID == 1 ? H1_IPV4 : CASTER_NODE_ID == 2 ? H2_IPV4 : CENTER_IPV4;
constexpr const char* kRole = CASTER_NODE_ID == 1 ? "H1" : CASTER_NODE_ID == 2 ? "H2" : "CENTER";
#endif
constexpr const char* kWheel = CASTER_WHEEL_ID;
inline EncoderConfig configuration() {
    EncoderConfig c{};
    c.role = CASTER_NODE_ID;
#if CASTER_NODE_ID >= 4
    c.ppr = 32; // Unused by ordinary acquisition; WHEEL_CPR is the x4 scale.
    c.ppr_verified = false;
    c.sign = SWIVEL_SIGN;
    c.glitch_filter_ns = PCNT_GLITCH_FILTER_NS;
    c.max_rpm = SWIVEL_MAX_RPM;
    c.max_rpm_verified = SWIVEL_MAX_RPM_VERIFIED;
    c.zero_count = 0;
    c.alpha_zero_rad = SWIVEL_ZERO_RAD;
    c.phase_reference_valid = SWIVEL_PHASE_REFERENCE_VALID;
    c.wheel_cpr = WHEEL_CPR;
    c.wheel_cpr_verified = WHEEL_CPR_VERIFIED;
    c.wheel_sign = WHEEL_SIGN;
    c.wheel_zero_count = WHEEL_ZERO_COUNT;
    c.wheel_velocity_window_us = WHEEL_VELOCITY_WINDOW_US;
#else
    c.ppr = c.role == 1 ? H1_PPR : H2_PPR;
    c.ppr_verified = c.role == 1 ? H1_PPR_VERIFIED : H2_PPR_VERIFIED;
    c.sign = c.role == 1 ? H1_SIGN : c.role == 2 ? H2_SIGN : CENTER_SIGN;
    c.glitch_filter_ns = PCNT_GLITCH_FILTER_NS;
    c.max_rpm = c.role == 1 ? H1_MAX_RPM : c.role == 2 ? H2_MAX_RPM : CENTER_MAX_RPM;
    c.max_rpm_verified = c.role == 1 ? H1_MAX_RPM_VERIFIED : c.role == 2 ? H2_MAX_RPM_VERIFIED : CENTER_MAX_RPM_VERIFIED;
    c.zero_count = c.role == 1 ? H1_ZERO_COUNT : H2_ZERO_COUNT;
    c.alpha_zero_rad = CENTER_ZERO_RAD;
    c.phase_reference_valid = c.role == 3 && CENTER_PHASE_REFERENCE_VALID;
#endif
    c.velocity_filter_tau_s = VELOCITY_FILTER_TAU_S;
    return c;
}
}  // namespace caster

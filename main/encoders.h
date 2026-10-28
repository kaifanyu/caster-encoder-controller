#pragma once

#include "encoder_math.h"

#include <cstdint>
#include <limits>

namespace caster {

struct EncoderConfig {
    int role;
    int ppr;
    bool ppr_verified;
    int sign;
    int glitch_filter_ns;
    double max_rpm;
    bool max_rpm_verified;
    int64_t zero_count;
    double alpha_zero_rad;
    bool phase_reference_valid;
    double velocity_filter_tau_s;
    // ORDINARY role: the existing coordinate settings above describe swivel.
    int wheel_cpr = 128;
    bool wheel_cpr_verified = false;
    int wheel_sign = 1;
    int64_t wheel_zero_count = 0;
    int64_t wheel_velocity_window_us = 50000;
};

enum StatusFlags : uint32_t {
    READY = 1u << 0,
    SAMPLE_VALID = 1u << 1,
    PPR_VERIFIED = 1u << 2,
    PHASE_REFERENCE_VALID = 1u << 3,
    UNWRAP_CONTINUOUS = 1u << 4,
    CLOCK_SYNC_VALID = 1u << 5,
    VELOCITY_VALID = 1u << 6,
    PARITY_ERROR = 1u << 7,
    CONTINUITY_LOST = 1u << 8,
    INDEX_VALID = 1u << 9,
    RATE_BOUND_VERIFIED = 1u << 10,
    ACQUISITION_OVERRUN = 1u << 11,
    SPI_IO_ERROR = 1u << 12,
    FORMAT_ERROR = 1u << 13,
};

constexpr double missing_value = std::numeric_limits<double>::quiet_NaN();

struct WheelReading {
    int64_t timestamp_us = 0;
    int64_t raw_count = 0;
    double angle_rad = missing_value;
    double velocity_rad_s = missing_value;
    double filtered_velocity_rad_s = missing_value;
    uint32_t flags = 0;
};

struct Reading {
    int64_t timestamp_us = 0;
    int64_t raw_count = 0;
    uint16_t raw_spi_word = 0;
    int position14 = -1;
    int position12 = -1;
    WheelReading wheel{};
    double angle_rad = missing_value;
    double absolute_angle_rad = missing_value;
    double calibrated_alpha_rad = missing_value;
    double velocity_rad_s = missing_value;
    double filtered_velocity_rad_s = missing_value;
    uint64_t index_event_count = 0;
    int64_t index_timestamp_us = 0;
    int64_t index_raw_count = 0;
    uint32_t flags = 0;
    uint32_t invalid_spi_responses = 0;
    uint32_t index_queue_drops = 0;
    // Internal serial diagnostics only; does not change the UDP schema.
    math::AbsoluteContinuityLoss continuity_loss{};
};

// Initialize once, after the >=250 ms stationary startup interval.
void encoder_init(const EncoderConfig& config);
// A single acquisition task owns reads; index GPIO interrupts run independently.
Reading encoder_read();
// Acquisition owner only: a fresh CENTER unwrap/velocity segment after prepare
// erasure. The PCNT cumulative count and hardware/index interrupts keep running.
void encoder_begin_recording_segment();

}  // namespace caster

#pragma once

#include "../main/encoder_math.h"
#include <array>
#include <cstddef>

namespace caster::ordinary {

constexpr int swivel_bins = 4096;
constexpr int64_t minimum_velocity_window_us = 1000;
constexpr int64_t maximum_velocity_window_us = 1000000;

// The AMT222A-V carries 12 significant bits in bits 13..2. Parity is
// calculated on the ORIGINAL 16-bit response, before shifting its position.
inline bool amt222a_valid(uint16_t word) {
    return math::amt25_parity_valid(word) && (word & 3u) == 0;
}

inline int position12(uint16_t word) { return (word & 0x3fffu) >> 2; }

inline double wrap_signed(double angle) {
    return math::wrap_positive(angle + math::tau / 2) - math::tau / 2;
}

inline bool swivel_delta(int now, int previous, int& delta) {
    delta = now - previous;
    if (delta == swivel_bins / 2 || delta == -swivel_bins / 2) return false;
    if (delta > swivel_bins / 2) delta -= swivel_bins;
    if (delta < -swivel_bins / 2) delta += swivel_bins;
    return true;
}

// Wrapped position remains independent of this tracker. Invalid SPI words
// must not call update(): the next valid read must justify the entire gap.
class SwivelTracker {
public:
    math::AbsoluteStep update(int position, int64_t timestamp_us, double max_rpm,
                             bool bound_verified, int sign, double zero_rad) {
        math::AbsoluteStep result;
        const bool bound_ok = bound_verified && std::isfinite(max_rpm) && max_rpm > 0;
        if (!initialized_) {
            initialized_ = true;
            continuous_ = bound_ok;
            if (!bound_ok) loss_.reason = math::AbsoluteLossReason::unverified_rate_bound;
            angle_ = wrap_signed(sign * (position * math::tau / swivel_bins - zero_rad));
        } else {
            const int64_t interval_us = timestamp_us - previous_time_;
            const double dt_s = interval_us / 1e6;
            const double allowed_bins = max_rpm / 60.0 * swivel_bins * dt_s;
            int delta = 0;
            const bool delta_ok = swivel_delta(position, previous_position_, delta);
            auto reason = math::AbsoluteLossReason::none;
            if (!bound_ok) reason = math::AbsoluteLossReason::unverified_rate_bound;
            else if (interval_us <= 0) reason = math::AbsoluteLossReason::invalid_time;
            else if (!(allowed_bins + 2 < swivel_bins / 2)) reason = math::AbsoluteLossReason::ambiguous_gap;
            else if (!delta_ok) reason = math::AbsoluteLossReason::half_turn;
            else if (!(std::abs(delta) <= allowed_bins + 2)) reason = math::AbsoluteLossReason::excessive_motion;
            const bool safe = reason == math::AbsoluteLossReason::none;
            if (continuous_ && !safe) loss_ = {reason, interval_us, delta};
            continuous_ = continuous_ && safe;
            if (safe) {
                result.velocity_valid = true;
                result.velocity_rad_s = sign * delta * math::tau / (swivel_bins * dt_s);
                if (continuous_) angle_ += sign * delta * math::tau / swivel_bins;
            }
        }
        previous_position_ = position;
        previous_time_ = timestamp_us;
        result.continuous = continuous_;
        if (continuous_) result.angle_rad = angle_;
        return result;
    }

    const math::AbsoluteContinuityLoss& continuity_loss() const { return loss_; }

private:
    bool initialized_ = false;
    bool continuous_ = false;
    int previous_position_ = 0;
    int64_t previous_time_ = 0;
    double angle_ = 0;
    math::AbsoluteContinuityLoss loss_{};
};

inline double wheel_angle(int64_t count, int64_t zero, int cpr, int sign) {
    return math::count_difference(count, zero) * sign * math::tau / cpr;
}

struct WheelVelocity {
    bool valid = false;
    double radians_per_second = std::numeric_limits<double>::quiet_NaN();
    int64_t interval_us = 0;
};

// A fresh trailing-window average at every acquisition, not a last-good value
// held across samples. The 128-CPR ring is too coarse for a useful 5-ms finite
// difference at low speed. This fixed-size history supports 1 s at 1000 Hz.
// It uses exact count subtraction before converting large totals to doubles.
class WheelVelocityEstimator {
public:
    void reset() { head_ = size_ = 0; }

    WheelVelocity update(int64_t count, int64_t timestamp_us, int cpr, int sign,
                         int64_t window_us) {
        WheelVelocity result;
        if (timestamp_us < 0 || cpr <= 0 || (sign != 1 && sign != -1) ||
            window_us < minimum_velocity_window_us || window_us > maximum_velocity_window_us) {
            reset();
            return result;
        }
        if (size_ != 0) {
            const int64_t gap = timestamp_us - at(size_ - 1).timestamp;
            // An interval longer than the requested window cannot describe a
            // fresh window. Start a new baseline rather than bridge that gap.
            if (gap <= 0 || gap > window_us) reset();
        }
        const int64_t target = timestamp_us - window_us;
        while (size_ >= 2 && at(1).timestamp <= target) {
            head_ = (head_ + 1) % history_.size();
            --size_;
        }
        if (size_ == history_.size()) reset();
        history_[(head_ + size_) % history_.size()] = {timestamp_us, count};
        ++size_;
        if (size_ < 2 || at(0).timestamp > target) return result;
        result.interval_us = timestamp_us - at(0).timestamp;
        result.radians_per_second = math::count_difference(count, at(0).count) *
            sign * math::tau / cpr / (result.interval_us / 1e6);
        result.valid = true;
        return result;
    }

private:
    struct Observation { int64_t timestamp = 0; int64_t count = 0; };
    const Observation& at(size_t offset) const { return history_[(head_ + offset) % history_.size()]; }
    std::array<Observation, 1024> history_{};
    size_t head_ = 0;
    size_t size_ = 0;
};

}  // namespace caster::ordinary

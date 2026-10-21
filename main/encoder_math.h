#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace caster::math {

constexpr double tau = 6.283185307179586476925286766559;
constexpr int absolute_bins = 16384;

inline bool amt25_parity_valid(uint16_t word) {
    unsigned odd = 0;
    unsigned even = 0;
    for (unsigned bit = 0; bit < 14; bit += 2) {
        even ^= (word >> bit) & 1u;
        odd ^= (word >> (bit + 1)) & 1u;
    }
    return ((word >> 15) & 1u) == (odd ^ 1u) &&
           ((word >> 14) & 1u) == (even ^ 1u);
}

// Arithmetic is explicitly modulo 2^32; never subtract signed int32 values.
// Caller must observe fewer than 2^31 edges between successful snapshots.
__attribute__((always_inline)) inline int64_t wrapped_delta32(uint32_t now, uint32_t before) {
    const uint32_t difference = now - before;
    return difference <= 0x7fffffffu ? static_cast<int64_t>(difference)
                                   : static_cast<int64_t>(difference) - 0x100000000LL;
}

class CountExtender {
public:
    __attribute__((always_inline)) inline int64_t update(uint32_t count) {
        total_ += wrapped_delta32(count, previous_);
        previous_ = count;
        return total_;
    }
    int64_t total() const { return total_; }
private:
    uint32_t previous_ = 0;
    int64_t total_ = 0;
};

inline double wrap_positive(double angle) {
    double result = std::fmod(angle, tau);
    return result < 0 ? result + tau : result;
}

inline bool absolute_delta(int now, int previous, int& delta) {
    delta = now - previous;
    if (delta == 8192 || delta == -8192) return false;
    if (delta > 8192) delta -= absolute_bins;
    if (delta < -8192) delta += absolute_bins;
    return true;
}

inline double low_pass(double previous, double value, double dt_s, double tau_s) {
    if (!std::isfinite(previous) || tau_s <= 0) return value;
    return previous + (-std::expm1(-dt_s / tau_s)) * (value - previous);
}

struct AbsoluteStep {
    bool continuous = false;
    bool velocity_valid = false;
    double angle_rad = std::numeric_limits<double>::quiet_NaN();
    double velocity_rad_s = std::numeric_limits<double>::quiet_NaN();
};

enum class AbsoluteLossReason {
    none,
    unverified_rate_bound,
    invalid_time,
    ambiguous_gap,
    half_turn,
    excessive_motion,
};

inline const char* absolute_loss_reason_name(AbsoluteLossReason reason) {
    switch (reason) {
        case AbsoluteLossReason::none: return "none";
        case AbsoluteLossReason::unverified_rate_bound: return "unverified_rate_bound";
        case AbsoluteLossReason::invalid_time: return "invalid_time";
        case AbsoluteLossReason::ambiguous_gap: return "ambiguous_gap";
        case AbsoluteLossReason::half_turn: return "half_turn";
        case AbsoluteLossReason::excessive_motion: return "excessive_motion";
    }
    return "unknown";
}

struct AbsoluteContinuityLoss {
    AbsoluteLossReason reason = AbsoluteLossReason::none;
    int64_t interval_us = 0;
    // Shortest wrapped candidate, not a recovered displacement across a gap.
    int delta_bins = 0;
};

// A verified *physical* speed bound is required to distinguish turns. Once a
// gap/rate violation makes integration ambiguous, continuity stays lost for
// this boot; valid single-turn phase remains usable independently.
class AbsoluteTracker {
public:
    AbsoluteStep update(int position, int64_t timestamp_us, double max_rpm,
                        bool bound_verified, int sign, double zero_rad) {
        AbsoluteStep result;
        const bool bound_ok = bound_verified && std::isfinite(max_rpm) && max_rpm > 0;
        if (!initialized_) {
            initialized_ = true;
            continuous_ = bound_ok;
            if (!bound_ok) loss_.reason = AbsoluteLossReason::unverified_rate_bound;
            angle_ = wrap_positive(sign * (position * tau / absolute_bins - zero_rad));
        } else {
            const int64_t interval_us = timestamp_us - previous_time_;
            const double dt_s = interval_us / 1e6;
            const double allowed_bins = max_rpm / 60.0 * absolute_bins * dt_s;
            int delta = 0;
            const bool delta_ok = absolute_delta(position, previous_position_, delta);
            // Two bins cover quantization near the limit; include that margin
            // in the half-turn test so a near-half-turn jump stays ambiguous.
            AbsoluteLossReason reason = AbsoluteLossReason::none;
            if (!bound_ok) reason = AbsoluteLossReason::unverified_rate_bound;
            else if (interval_us <= 0) reason = AbsoluteLossReason::invalid_time;
            else if (!(allowed_bins + 2 < 8192)) reason = AbsoluteLossReason::ambiguous_gap;
            else if (!delta_ok) reason = AbsoluteLossReason::half_turn;
            else if (!(std::abs(delta) <= allowed_bins + 2)) reason = AbsoluteLossReason::excessive_motion;
            const bool safe = reason == AbsoluteLossReason::none;
            if (continuous_ && !safe) loss_ = {reason, interval_us, delta};
            continuous_ = continuous_ && safe;
            if (safe) {
                result.velocity_valid = true;
                result.velocity_rad_s = sign * delta * tau / (absolute_bins * dt_s);
                if (continuous_) angle_ += sign * delta * tau / absolute_bins;
            }
        }
        previous_position_ = position;
        previous_time_ = timestamp_us;
        result.continuous = continuous_;
        if (continuous_) result.angle_rad = angle_;
        return result;
    }

    const AbsoluteContinuityLoss& continuity_loss() const { return loss_; }

private:
    bool initialized_ = false;
    bool continuous_ = false;
    int previous_position_ = 0;
    int64_t previous_time_ = 0;
    double angle_ = 0;
    AbsoluteContinuityLoss loss_{};
};

}  // namespace caster::math

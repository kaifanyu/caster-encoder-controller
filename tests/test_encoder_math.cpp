#include "encoder_math.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace caster::math;

namespace {
bool close(double actual, double expected, double tolerance = 1e-10) {
    return std::abs(actual - expected) < tolerance;
}

void parity_checks() {
    assert(amt25_parity_valid(0x61ab));
    assert((0x61ab & 0x3fff) == 8619);
    assert(amt25_parity_valid(0xc000));
    assert(!amt25_parity_valid(0x0000));
    assert(!amt25_parity_valid(0xffff));
    for (const uint16_t word : {uint16_t{0x61ab}, uint16_t{0xc000}}) {
        for (unsigned bit = 0; bit < 16; ++bit) {
            assert(!amt25_parity_valid(word ^ (1u << bit)));
        }
    }
    // Exactly one choice of the two check bits must pass for each phase.
    for (unsigned phase = 0; phase < 16384; ++phase) {
        unsigned valid = 0;
        for (unsigned check = 0; check < 4; ++check) {
            valid += amt25_parity_valid(static_cast<uint16_t>(phase | (check << 14)));
        }
        assert(valid == 1);
    }
}

void count_rollover_checks() {
    CountExtender forward;
    assert(forward.update(0) == 0);
    assert(forward.update(0x7ffffffe) == 2147483646LL);
    assert(forward.update(0x80000002) == 2147483650LL);
    assert(forward.update(0xfffffffe) == 4294967294LL);
    assert(forward.update(3) == 4294967299LL);
    assert(forward.update(0xfffffffd) == 4294967293LL);
    CountExtender backward;
    assert(backward.update(0xfffffffe) == -2);
    assert(backward.update(0x80000002) == -2147483646LL);
    assert(backward.update(0x7ffffffe) == -2147483650LL);
    assert(backward.update(0) == -4294967296LL);
    assert(backward.update(0xfffffffc) == -4294967300LL);
    assert(wrapped_delta32(0, 0xffffffff) == 1);
    assert(wrapped_delta32(0xffffffff, 0) == -1);
}

void absolute_wrap_checks() {
    int delta = 0;
    assert(absolute_delta(0, 16383, delta) && delta == 1);
    assert(absolute_delta(16383, 0, delta) && delta == -1);
    assert(!absolute_delta(8192, 0, delta));
    assert(!absolute_delta(0, 8192, delta));

    AbsoluteTracker forward;
    auto first = forward.update(16380, 1000, 600, true, 1, 0);
    assert(first.continuous && !first.velocity_valid);
    auto second = forward.update(4, 6000, 600, true, 1, 0);
    assert(second.continuous && second.velocity_valid);
    assert(close(second.angle_rad - first.angle_rad, 8 * tau / 16384));
    assert(close(second.velocity_rad_s, 8 * tau / 16384 / .005));
    assert(second.angle_rad > tau);

    AbsoluteTracker backward;
    first = backward.update(4, 1000, 600, true, 1, 0);
    second = backward.update(16380, 6000, 600, true, 1, 0);
    assert(second.continuous && second.velocity_valid);
    assert(close(second.angle_rad - first.angle_rad, -8 * tau / 16384));
    assert(second.angle_rad < 0);

    AbsoluteTracker reversed;
    first = reversed.update(4, 1000, 600, true, -1, .1);
    second = reversed.update(12, 6000, 600, true, -1, .1);
    assert(close(first.angle_rad, wrap_positive(.1 - 4 * tau / 16384)));
    assert(second.angle_rad < first.angle_rad && second.velocity_rad_s < 0);
}

void ambiguity_checks() {
    AbsoluteTracker unverified;
    auto step = unverified.update(1234, 1000, 600, false, 1, 0);
    assert(!step.continuous && !step.velocity_valid && std::isnan(step.angle_rad));
    step = unverified.update(1235, 6000, 600, false, 1, 0);
    assert(!step.continuous && !step.velocity_valid && std::isnan(step.velocity_rad_s));

    AbsoluteTracker gap;
    gap.update(0, 1000, 600, true, 1, 0);
    // No updates for lost/invalid words: the full gap must satisfy the bound.
    step = gap.update(1000, 41000, 600, true, 1, 0);
    assert(step.continuous && step.velocity_valid);
    assert(close(step.velocity_rad_s, 1000 * tau / 16384 / .040));
    step = gap.update(1001, 91000, 600, true, 1, 0);
    assert(!step.continuous && !step.velocity_valid && std::isnan(step.angle_rad));
    step = gap.update(1002, 96000, 600, true, 1, 0);
    assert(!step.continuous && step.velocity_valid && std::isnan(step.angle_rad));

    AbsoluteTracker impossible_speed;
    impossible_speed.update(0, 1000, 60, true, 1, 0);
    step = impossible_speed.update(1000, 6000, 60, true, 1, 0);
    assert(!step.continuous && !step.velocity_valid);

    AbsoluteTracker half_turn;
    half_turn.update(0, 1000, 600, true, 1, 0);
    step = half_turn.update(8192, 6000, 600, true, 1, 0);
    assert(!step.continuous && !step.velocity_valid);

    AbsoluteTracker duplicate_time;
    duplicate_time.update(0, 1000, 600, true, 1, 0);
    step = duplicate_time.update(0, 1000, 600, true, 1, 0);
    assert(!step.continuous && !step.velocity_valid);
}

void expect_loss(const AbsoluteTracker& tracker, AbsoluteLossReason reason,
                 int64_t interval_us, int delta_bins) {
    const auto& loss = tracker.continuity_loss();
    assert(loss.reason == reason);
    assert(loss.interval_us == interval_us);
    assert(loss.delta_bins == delta_bins);
}

void expect_invalid_step(const AbsoluteStep& step) {
    assert(!step.continuous && !step.velocity_valid);
    assert(std::isnan(step.angle_rad) && std::isnan(step.velocity_rad_s));
}

void expect_velocity_recovery(AbsoluteTracker& tracker, int position,
                              int64_t timestamp_us) {
    // Copy the record so later updates cannot hide an accidental overwrite.
    const auto first_loss = tracker.continuity_loss();
    const int next = (position + 1) % absolute_bins;
    auto step = tracker.update(next, timestamp_us + 5000, 600, true, 1, 0);
    assert(!step.continuous && step.velocity_valid && std::isnan(step.angle_rad));
    assert(close(step.velocity_rad_s, tau / absolute_bins / .005));
    expect_loss(tracker, first_loss.reason, first_loss.interval_us, first_loss.delta_bins);

    // A different failure must not replace the explanation for first loss.
    step = tracker.update(next, timestamp_us + 4000, 600, true, 1, 0);
    expect_invalid_step(step);
    expect_loss(tracker, first_loss.reason, first_loss.interval_us, first_loss.delta_bins);
}

void continuity_diagnostic_checks() {
    AbsoluteTracker healthy;
    expect_loss(healthy, AbsoluteLossReason::none, 0, 0);
    auto step = healthy.update(100, 1000, 600, true, 1, 0);
    assert(step.continuous && !step.velocity_valid && std::isfinite(step.angle_rad));
    expect_loss(healthy, AbsoluteLossReason::none, 0, 0);
    step = healthy.update(101, 6000, 600, true, 1, 0);
    assert(step.continuous && step.velocity_valid);
    expect_loss(healthy, AbsoluteLossReason::none, 0, 0);

    AbsoluteTracker unverified;
    expect_invalid_step(unverified.update(100, 1000, 600, false, 1, 0));
    expect_loss(unverified, AbsoluteLossReason::unverified_rate_bound, 0, 0);
    expect_velocity_recovery(unverified, 100, 1000);

    for (const double max_rpm : {0.0, -600.0,
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
        AbsoluteTracker invalid_bound;
        expect_invalid_step(invalid_bound.update(100, 1000, max_rpm, true, 1, 0));
        expect_loss(invalid_bound, AbsoluteLossReason::unverified_rate_bound, 0, 0);
        expect_velocity_recovery(invalid_bound, 100, 1000);
    }

    AbsoluteTracker revoked_bound;
    revoked_bound.update(16383, 1000, 600, true, 1, 0);
    expect_invalid_step(revoked_bound.update(0, 6000, 600, false, 1, 0));
    expect_loss(revoked_bound, AbsoluteLossReason::unverified_rate_bound, 5000, 1);
    expect_velocity_recovery(revoked_bound, 0, 6000);

    for (const int64_t interval_us : {int64_t{0}, int64_t{-5000}}) {
        AbsoluteTracker invalid_time;
        invalid_time.update(16383, 6000, 600, true, 1, 0);
        expect_invalid_step(invalid_time.update(0, 6000 + interval_us, 600, true, 1, 0));
        expect_loss(invalid_time, AbsoluteLossReason::invalid_time, interval_us, 1);
        expect_velocity_recovery(invalid_time, 0, 6000 + interval_us);
    }

    AbsoluteTracker gap;
    gap.update(16380, 1000, 600, true, 1, 0);
    expect_invalid_step(gap.update(4, 51000, 600, true, 1, 0));
    expect_loss(gap, AbsoluteLossReason::ambiguous_gap, 50000, 8);
    expect_velocity_recovery(gap, 4, 51000);

    // The quantization margin also makes intervals just below 50 ms ambiguous.
    AbsoluteTracker gap_margin;
    gap_margin.update(0, 1000, 600, true, 1, 0);
    expect_invalid_step(gap_margin.update(1, 50988, 600, true, 1, 0));
    expect_loss(gap_margin, AbsoluteLossReason::ambiguous_gap, 49988, 1);
    expect_velocity_recovery(gap_margin, 1, 50988);

    for (const int position : {0, 8192}) {
        AbsoluteTracker half_turn;
        half_turn.update(8192 - position, 1000, 600, true, 1, 0);
        expect_invalid_step(half_turn.update(position, 6000, 600, true, 1, 0));
        expect_loss(half_turn, AbsoluteLossReason::half_turn, 5000, 2 * position - 8192);
        expect_velocity_recovery(half_turn, position, 6000);
    }

    for (const int position : {1000, 15384}) {
        AbsoluteTracker excessive_motion;
        const int previous = position == 1000 ? 16380 : 4;
        excessive_motion.update(previous, 1000, 60, true, 1, 0);
        expect_invalid_step(excessive_motion.update(position, 6000, 60, true, 1, 0));
        expect_loss(excessive_motion, AbsoluteLossReason::excessive_motion,
                    5000, position == 1000 ? 1004 : -1004);
        expect_velocity_recovery(excessive_motion, position, 6000);
    }
}
}  // namespace

int main() {
    parity_checks();
    count_rollover_checks();
    absolute_wrap_checks();
    ambiguity_checks();
    continuity_diagnostic_checks();
    assert(close(wrap_positive(-.25), tau - .25));
    assert(close(low_pass(0, 10, .1, .1), 10 * (1 - std::exp(-1))));
    assert(low_pass(0, 10, .1, 0) == 10);
    std::cout << "Encoder math tests passed\n";
}

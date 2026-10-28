#include "../encoder_math.h"
#include <cassert>
#include <iostream>

using namespace caster;

namespace {
bool close(double a, double b, double tolerance = 1e-10) { return std::abs(a - b) < tolerance; }

uint16_t response(int position) {
    const auto data = static_cast<uint16_t>(position << 2);
    for (unsigned check = 0; check < 4; ++check) {
        const auto word = static_cast<uint16_t>(data | (check << 14));
        if (ordinary::amt222a_valid(word)) return word;
    }
    assert(false);
    return 0;
}

void parity_and_format() {
    assert(ordinary::amt222a_valid(0xc000));
    assert(ordinary::amt222a_valid(0xfffc));
    assert(ordinary::position12(0xfffc) == 4095);
    assert(!ordinary::amt222a_valid(0));
    assert(!ordinary::amt222a_valid(0xffff));
    // Manufacturer's valid 14-bit example is NOT a valid 12-bit response.
    assert(math::amt25_parity_valid(0x61ab));
    assert(!ordinary::amt222a_valid(0x61ab));
    // Independent byte-based check against the AMT22 datasheet K1/K0 formula.
    // Generating test words with the production validator alone could hide a
    // consistently wrong parity implementation.
    for (unsigned raw = 0; raw <= 0xffff; ++raw) {
        const auto bit = [raw](unsigned n) { return (raw >> n) & 1u; };
        const unsigned k1 = 1u ^ bit(13) ^ bit(11) ^ bit(9) ^ bit(7) ^ bit(5) ^ bit(3) ^ bit(1);
        const unsigned k0 = 1u ^ bit(12) ^ bit(10) ^ bit(8) ^ bit(6) ^ bit(4) ^ bit(2) ^ bit(0);
        const bool valid = bit(15) == k1 && bit(14) == k0;
        assert(math::amt25_parity_valid(static_cast<uint16_t>(raw)) == valid);
        assert(ordinary::amt222a_valid(static_cast<uint16_t>(raw)) == (valid && (raw & 3u) == 0));
    }
    for (int position = 0; position < 4096; ++position) {
        const auto word = response(position);
        assert(ordinary::position12(word) == position);
        for (unsigned bit = 0; bit < 16; ++bit)
            assert(!ordinary::amt222a_valid(word ^ (1u << bit)));
    }
}

void swivel_motion() {
    int delta = 0;
    assert(ordinary::swivel_delta(0, 4095, delta) && delta == 1);
    assert(ordinary::swivel_delta(4095, 0, delta) && delta == -1);
    assert(!ordinary::swivel_delta(2048, 0, delta));
    assert(!ordinary::swivel_delta(0, 2048, delta));
    assert(close(ordinary::wrap_signed(math::tau / 2), -math::tau / 2));
    for (int sign : {-1, 1}) {
        ordinary::SwivelTracker tracker;
        auto first = tracker.update(4092, 1000, 600, true, sign, .2);
        assert(first.continuous && !first.velocity_valid);
        for (int i = 1; i <= 256; ++i) {
            const auto step = tracker.update((4092 + 32 * i) % 4096,
                1000 + i * 15625LL / 2, 600, true, sign, .2);
            assert(step.continuous && step.velocity_valid);
            assert(close(step.angle_rad - first.angle_rad, sign * i * 32 * math::tau / 4096));
            assert(close(step.velocity_rad_s, sign * math::tau, .001));
        }
    }
}

void swivel_gaps_and_recovery() {
    ordinary::SwivelTracker tracker;
    auto step = tracker.update(0, 1000, 600, false, 1, 0);
    assert(!step.continuous && !step.velocity_valid && std::isnan(step.angle_rad));
    tracker = ordinary::SwivelTracker{};
    tracker.update(4095, 1000, 600, true, 1, 0);
    step = tracker.update(0, 6000, 600, true, 1, 0);
    assert(step.continuous && step.velocity_valid);
    // At 600 RPM, 50 ms can conceal >= half a turn; never infer those turns.
    step = tracker.update(1, 56000, 600, true, 1, 0);
    assert(!step.continuous && !step.velocity_valid && std::isnan(step.angle_rad));
    assert(tracker.continuity_loss().reason == math::AbsoluteLossReason::ambiguous_gap);
    step = tracker.update(2, 61000, 600, true, 1, 0);
    assert(!step.continuous && step.velocity_valid && std::isnan(step.angle_rad));
    assert(tracker.continuity_loss().interval_us == 50000);
    step = tracker.update(2, 61000, 600, true, 1, 0);
    assert(!step.velocity_valid);
    assert(tracker.continuity_loss().reason == math::AbsoluteLossReason::ambiguous_gap);
    tracker = ordinary::SwivelTracker{};
    tracker.update(0, 1000, 600, true, 1, 0);
    step = tracker.update(2048, 6000, 600, true, 1, 0);
    assert(!step.continuous && !step.velocity_valid);
    assert(tracker.continuity_loss().reason == math::AbsoluteLossReason::half_turn);
    tracker = ordinary::SwivelTracker{};
    tracker.update(0, 1000, 60, true, 1, 0);
    step = tracker.update(1000, 6000, 60, true, 1, 0);
    assert(!step.continuous && !step.velocity_valid);
    assert(tracker.continuity_loss().reason == math::AbsoluteLossReason::excessive_motion);
}

void wheel_measurement() {
    assert(close(ordinary::wheel_angle(128 + 23, 23, 128, 1), math::tau));
    assert(close(ordinary::wheel_angle(-128 + 23, 23, 128, -1), math::tau));
    const int64_t large = int64_t{1} << 54;
    assert(close(ordinary::wheel_angle(large + 1, large, 128, 1), math::tau / 128));
    for (int sign : {-1, 1}) {
        ordinary::WheelVelocityEstimator velocity;
        auto step = velocity.update(large, 1000, 128, sign, 50000);
        assert(!step.valid && std::isnan(step.radians_per_second));
        for (int i = 1; i <= 10; ++i) {
            step = velocity.update(large + i / 10, 1000 + i * 5000, 128, sign, 50000);
            assert(step.valid == (i == 10));
        }
        // One count over 50 ms, rather than a 5-ms 9.82-rad/s spike.
        assert(step.interval_us == 50000);
        assert(close(step.radians_per_second, sign * math::tau / 128 / .05));
        step = velocity.update(large + 1, 56000, 128, sign, 50000);
        assert(step.valid && step.interval_us == 50000);
        // Missing a full window invalidates velocity and restarts its history.
        step = velocity.update(large + 3, 120000, 128, sign, 50000);
        assert(!step.valid);
        step = velocity.update(large + 4, 170000, 128, sign, 50000);
        assert(step.valid);
        // Time must increase, including a duplicate-timestamp input.
        assert(!velocity.update(large + 4, 170000, 128, sign, 50000).valid);
        velocity.reset();
        assert(!velocity.update(large + 4, 180000, 128, sign, 50000).valid);
    }
    ordinary::WheelVelocityEstimator velocity;
    for (int i = 0; i <= 3000; ++i) {
        const auto step = velocity.update(i, i * 1000LL, 128, 1, 1000000);
        assert(step.valid == (i >= 1000));
        if (step.valid) assert(close(step.radians_per_second, 1000 * math::tau / 128));
    }
    assert(!velocity.update(1, 4000000, 0, 1, 50000).valid);
    assert(!velocity.update(1, 4000000, 128, 0, 50000).valid);
    assert(!velocity.update(1, 4000000, 128, 1, 1000001).valid);
}
}

int main() {
    parity_and_format();
    swivel_motion();
    swivel_gaps_and_recovery();
    wheel_measurement();
    std::cout << "Ordinary caster math checks passed\n";
}

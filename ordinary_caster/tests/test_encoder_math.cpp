#include "../encoder_math.h"
#include <cassert>
#include <iostream>
#include <limits>

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

void unavailable(const ordinary::WheelVelocity& value) {
    assert(!value.valid && std::isnan(value.radians_per_second));
}

int64_t trajectory(int rate, int64_t timestamp_us, int phase, int direction = 1) {
    // Independent, exact quantization of constant physical motion. Phase uses
    // millionths of a count; no production estimator participates in this input.
    return direction * ((rate * timestamp_us + phase) / 1000000);
}

void wheel_measurement() {
    assert(close(ordinary::wheel_angle(128 + 23, 23, 128, 1), math::tau));
    assert(close(ordinary::wheel_angle(-128 + 23, 23, 128, -1), math::tau));
    const int64_t large = int64_t{1} << 54;
    assert(close(ordinary::wheel_angle(large + 1, large, 128, 1), math::tau / 128));

    // A first observed edge cannot reveal the time of a preceding edge.
    // After a stationary baseline it must not invent a 50-ms moving speed.
    for (int sign : {-1, 1}) {
        ordinary::WheelVelocityEstimator velocity;
        unavailable(velocity.update(large, 0, 128, sign, 50000));
        for (int i = 1; i <= 10; ++i) {
            const auto step = velocity.update(large, i * 5000, 128, sign, 50000);
            assert(step.valid == (i == 10));
            if (step.valid) assert(step.radians_per_second == 0);
        }
        unavailable(velocity.update(large + 1, 55000, 128, sign, 50000));
        for (int64_t t = 60000; t <= 100000; t += 5000)
            unavailable(velocity.update(large + 1, t, 128, sign, 50000));
        auto step = velocity.update(large + 2, 105000, 128, sign, 50000);
        assert(step.valid);
        assert(close(step.radians_per_second, sign * math::tau / 128 / .05));
        for (int64_t t = 110000; t < 355000; t += 5000) {
            step = velocity.update(large + 2, t, 128, sign, 50000);
            assert(step.valid && sign * step.radians_per_second > 0);
            const double cap = math::tau / 128 / ((t - 105000) / 1e6);
            assert(std::abs(step.radians_per_second) <= cap + 1e-10);
        }
        step = velocity.update(large + 2, 355000, 128, sign, 50000);
        assert(step.valid && step.radians_per_second == 0);

        // Timeout discards edge timing; restarting needs two fresh changes.
        unavailable(velocity.update(large + 3, 360000, 128, sign, 50000));
        step = velocity.update(large + 4, 365000, 128, sign, 50000);
        assert(step.valid && close(step.radians_per_second, sign * 200 * math::tau / 128));
        // The first reverse change invalidates the old-direction estimate.
        unavailable(velocity.update(large + 3, 370000, 128, sign, 50000));
        step = velocity.update(large + 2, 375000, 128, sign, 50000);
        assert(step.valid && close(step.radians_per_second, -sign * 200 * math::tau / 128));
    }
}

void wheel_sparse_resolution() {
    for (int rate : {5, 10, 37}) {
        for (int phase : {0, 250000, 750000}) {
            for (int direction : {-1, 1}) {
                for (int sign : {-1, 1}) {
                    ordinary::WheelVelocityEstimator velocity;
                    const double expected = direction * sign * rate * math::tau / 128;
                    double sum = 0, squared_error = 0, old_squared_error = 0;
                    int samples = 0;
                    for (int64_t t = 0; t <= 8000000; t += 5000) {
                        const int64_t count = trajectory(rate, t, phase, direction);
                        const auto step = velocity.update(count, t, 128, sign, 50000);
                        if (t < 2000000) continue; // exclude startup from steady-state metrics
                        assert(step.valid && std::isfinite(step.radians_per_second));
                        assert(direction * sign * step.radians_per_second > 0);
                        const double old = sign * (count - trajectory(rate, t - 50000, phase, direction)) *
                            math::tau / 128 / .05;
                        sum += step.radians_per_second;
                        squared_error += std::pow(step.radians_per_second - expected, 2);
                        old_squared_error += std::pow(old - expected, 2);
                        ++samples;
                    }
                    assert(std::abs(sum / samples - expected) < .10 * std::abs(expected));
                    // Compare against the former fixed-window estimate, not
                    // against another copy of the new estimator's algorithm.
                    assert(squared_error < old_squared_error);
                    if (rate <= 10) assert(squared_error < .25 * old_squared_error);
                }
            }
        }
    }
}

void wheel_fast_jitter_and_large_counts() {
    for (int rate : {10, 37, 2000}) {
        ordinary::WheelVelocityEstimator velocity;
        const int64_t jitter[] = {4000, 6000, 4500, 5500};
        double sum = 0;
        int samples = 0;
        for (int i = 0, t = 0; t <= 8000000; ++i) {
            const auto step = velocity.update(trajectory(rate, t, 250000), t, 128, 1, 50000);
            if (t >= 2000000) {
                assert(step.valid && std::isfinite(step.radians_per_second));
                sum += step.radians_per_second;
                ++samples;
                if (rate == 2000) assert(close(step.radians_per_second, rate * math::tau / 128));
            }
            // A single missed acquisition is not a full-window outage.
            t += (i == 600 ? 20000 : jitter[i % 4]);
        }
        assert(std::abs(sum / samples - rate * math::tau / 128) < .10 * rate * math::tau / 128);
    }

    for (int64_t offset : {int64_t{1} << 54, -(int64_t{1} << 54),
                           std::numeric_limits<int64_t>::max() - 10000,
                           std::numeric_limits<int64_t>::min() + 10000}) {
        for (int direction : {-1, 1}) {
            ordinary::WheelVelocityEstimator relative, translated;
            for (int64_t t = 0; t <= 2000000; t += 5000) {
                const auto count = trajectory(10, t, 750000, direction);
                const auto a = relative.update(count, t, 128, 1, 50000);
                const auto b = translated.update(offset + count, t, 128, 1, 50000);
                assert(a.valid == b.valid);
                if (a.valid) assert(close(a.radians_per_second, b.radians_per_second));
                else unavailable(b);
            }
        }
    }
}

void wheel_timeouts_gaps_and_configuration() {
    ordinary::WheelVelocityEstimator velocity;
    unavailable(velocity.update(0, 0, 128, 1, 50000));
    unavailable(velocity.update(1, 5000, 128, 1, 50000));
    assert(velocity.update(2, 10000, 128, 1, 50000).valid);
    for (int64_t t = 15000; t < 260000; t += 5000)
        assert(velocity.update(2, t, 128, 1, 50000).valid);
    // A new edge at exactly the deadline belongs to continuous motion if no
    // earlier stationary sample has already expired the preceding history.
    auto step = velocity.update(3, 260000, 128, 1, 50000);
    assert(step.valid && close(step.radians_per_second, math::tau / 128 / .25));

    // A gap exactly equal to the window is allowed; a longer one resets.
    assert(velocity.update(4, 310000, 128, 1, 50000).valid);
    unavailable(velocity.update(10, 360001, 128, 1, 50000));
    unavailable(velocity.update(11, 365001, 128, 1, 50000));
    assert(velocity.update(12, 370001, 128, 1, 50000).valid);
    unavailable(velocity.update(12, 370001, 128, 1, 50000));
    unavailable(velocity.update(13, 375001, 128, 1, 50000));
    assert(velocity.update(14, 380001, 128, 1, 50000).valid);
    unavailable(velocity.update(14, 379000, 128, 1, 50000));
    velocity.reset();
    unavailable(velocity.update(14, 400000, 128, 1, 50000));

    struct Configuration { int cpr, sign; int64_t window, timeout; };
    for (const auto config : {Configuration{256, 1, 50000, 250000},
                              Configuration{128, -1, 50000, 250000},
                              Configuration{128, 1, 100000, 250000},
                              Configuration{128, 1, 50000, 500000}}) {
        velocity.reset();
        velocity.update(0, 0, 128, 1, 50000);
        velocity.update(1, 5000, 128, 1, 50000);
        assert(velocity.update(2, 10000, 128, 1, 50000).valid);
        unavailable(velocity.update(3, 15000, config.cpr, config.sign, config.window, config.timeout));
        unavailable(velocity.update(4, 20000, config.cpr, config.sign, config.window, config.timeout));
        step = velocity.update(5, 25000, config.cpr, config.sign, config.window, config.timeout);
        assert(step.valid && close(step.radians_per_second, config.sign * 200 * math::tau / config.cpr));
    }

    for (const auto config : {Configuration{0, 1, 50000, 250000},
                              Configuration{-1, 1, 50000, 250000},
                              Configuration{128, 0, 50000, 250000},
                              Configuration{128, 2, 50000, 250000},
                              Configuration{128, 1, 999, 250000},
                              Configuration{128, 1, 1000001, 1000001},
                              Configuration{128, 1, 50000, 49999},
                              Configuration{128, 1, 50000, 1000001}}) {
        unavailable(velocity.update(6, 30000, config.cpr, config.sign, config.window, config.timeout));
        unavailable(velocity.update(7, 35000, 128, 1, 50000));
        unavailable(velocity.update(8, 40000, 128, 1, 50000));
        assert(velocity.update(9, 45000, 128, 1, 50000).valid);
    }
    unavailable(velocity.update(10, -1, 128, 1, 50000));

    // Exercise the largest history at the supported maximum sample rate.
    velocity.reset();
    for (int i = 0; i <= 3000; ++i) {
        step = velocity.update(i, i * 1000LL, 128, 1, 1000000, 1000000);
        assert(step.valid == (i >= 2));
        if (step.valid) assert(close(step.radians_per_second, 1000 * math::tau / 128));
    }
}
}

int main() {
    parity_and_format();
    swivel_motion();
    swivel_gaps_and_recovery();
    wheel_measurement();
    wheel_sparse_resolution();
    wheel_fast_jitter_and_large_counts();
    wheel_timeouts_gaps_and_configuration();
    std::cout << "Ordinary caster math checks passed\n";
}

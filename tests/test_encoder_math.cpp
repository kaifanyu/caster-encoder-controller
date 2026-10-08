#include "encoder_math.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

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
}  // namespace

int main() {
    parity_checks();
    count_rollover_checks();
    absolute_wrap_checks();
    ambiguity_checks();
    assert(close(wrap_positive(-.25), tau - .25));
    assert(close(low_pass(0, 10, .1, .1), 10 * (1 - std::exp(-1))));
    assert(low_pass(0, 10, .1, 0) == 10);
    std::cout << "Encoder math tests passed\n";
}

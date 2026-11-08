#include "stubs/encoder_sdk.h"
#include "../../main/encoders.h"
#include <cmath>
#include <cstdlib>

int main(int argc, char** argv) {
    using namespace caster;
    assert(argc == 2 || argc == 3);
    const int role = std::atoi(argv[1]);
    assert(role == 5 || role == 6);
    EncoderConfig config{};
    config.role = role;
    config.sign = config.wheel_sign = 1;
    config.max_rpm = 600;
    config.max_rpm_verified = argc == 2;
    config.wheel_cpr_verified = role != 6; // unverified scale must remain explicit
    encoder_init(config);
    assert(mock_pcnt_init_count == (role == 6));
    assert(mock_spi_init_count == (role == 5));
    // Force the unused peripheral to fail; the active sensor must still work.
    mock_spi_error = mock_bus_error = role == 6;
    mock_counter_error = role == 5;
    mock_word = 0xc000;
    for (int i = 0; i < 12; ++i) {
        mock_time = 110000 + i * 5000;
        mock_count = i;
        const auto r = encoder_read();
        if (role == 5) {
            assert(r.flags & SAMPLE_VALID);
            assert(r.position12 == 0 && r.wheel.flags == 0);
            assert(r.wheel.timestamp_us == 0 && mock_pcnt_reads == 0);
            assert(std::isnan(r.wheel.angle_rad));
            assert(std::isfinite(r.calibrated_alpha_rad));
            if (config.max_rpm_verified && i > 0) {
                assert((r.flags & VELOCITY_VALID) && r.velocity_rad_s == 0);
            } else {
                assert(!(r.flags & VELOCITY_VALID) && std::isnan(r.velocity_rad_s));
            }
        } else {
            assert(r.wheel.flags & SAMPLE_VALID);
            assert(r.timestamp_us == r.wheel.timestamp_us);
            assert(r.wheel.raw_count == i && r.flags == 0);
            assert(r.position12 == -1 && mock_spi_reads == 0 && r.invalid_spi_responses == 0);
            assert(std::isnan(r.calibrated_alpha_rad));
        }
    }
    if (role == 5) {
        mock_word = 0;
        mock_time += 5000;
        const auto failed = encoder_read();
        // Exact reported 0x0081 when the rate bound is unverified.
        const uint32_t expected = READY | PARITY_ERROR |
                                  (config.max_rpm_verified ? RATE_BOUND_VERIFIED : 0u);
        assert(failed.flags == expected && failed.raw_spi_word == 0 && failed.position12 == -1);
        assert(std::isnan(failed.calibrated_alpha_rad) && std::isnan(failed.velocity_rad_s));
        mock_word = 0xc000;
    }
    encoder_begin_recording_segment();
    mock_time += 5000;
    const auto r = encoder_read();
    assert(role == 5 ? bool(r.flags & SAMPLE_VALID) : r.wheel.raw_count == 11);
    if (role == 6) {
        assert(!(r.wheel.flags & VELOCITY_VALID));
        // Exercise the real driver's flags/filter plumbing at one count every
        // 100 ms: the old 50-ms window oscillated between zero and 0.982 rad/s.
        const double slow_speed = 2 * std::acos(-1.0) / 128 / .1;
        for (int i = 1; i <= 60; ++i) {
            mock_time += 5000;
            mock_count = 11 + i / 20;
            const auto sample = encoder_read();
            assert(sample.wheel.raw_count == mock_count && (sample.wheel.flags & SAMPLE_VALID));
            assert(!(sample.wheel.flags & PPR_VERIFIED));
            assert(mock_spi_reads == 0 && sample.flags == 0);
            if (i >= 20 && i < 40) {
                assert(!(sample.wheel.flags & VELOCITY_VALID));
                assert(std::isnan(sample.wheel.velocity_rad_s));
                assert(std::isnan(sample.wheel.filtered_velocity_rad_s));
            } else if (i >= 40) {
                assert(sample.wheel.flags & VELOCITY_VALID);
                assert(std::abs(sample.wheel.velocity_rad_s - slow_speed) < 1e-10);
                assert(sample.wheel.filtered_velocity_rad_s == sample.wheel.velocity_rad_s);
            }
        }
        for (int i = 1; i <= 50; ++i) {
            mock_time += 5000;
            const auto sample = encoder_read();
            assert(sample.wheel.flags & VELOCITY_VALID);
            assert(sample.wheel.velocity_rad_s >= 0 && sample.wheel.velocity_rad_s <= slow_speed);
            if (i == 50) assert(sample.wheel.velocity_rad_s == 0);
        }
        // An isolated first reverse change must clear both raw and filtered
        // velocity validity; a second change establishes the new direction.
        mock_time += 5000;
        --mock_count;
        const auto first = encoder_read();
        assert(!(first.wheel.flags & VELOCITY_VALID));
        assert(std::isnan(first.wheel.velocity_rad_s));
        assert(std::isnan(first.wheel.filtered_velocity_rad_s));
        mock_time += 5000;
        --mock_count;
        const auto second = encoder_read();
        assert((second.wheel.flags & VELOCITY_VALID) && second.wheel.velocity_rad_s < 0);
        assert(second.wheel.filtered_velocity_rad_s == second.wheel.velocity_rad_s);
    }
}

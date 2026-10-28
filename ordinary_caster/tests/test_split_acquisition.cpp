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
    config.wheel_cpr_verified = true;
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
}

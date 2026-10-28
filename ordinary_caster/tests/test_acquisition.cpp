#include "stubs/encoder_sdk.h"
#include "../../main/encoders.h"
#include "../encoder_math.h"
#include <iostream>

using namespace caster;

uint16_t response(int position) {
    for (unsigned check = 0; check < 4; ++check) {
        const auto word = static_cast<uint16_t>((position << 2) | (check << 14));
        if (ordinary::amt222a_valid(word)) return word;
    }
    assert(false); return 0;
}

Reading read_at(int64_t timestamp, int count, uint16_t word) {
    mock_time = timestamp;
    mock_count = count;
    mock_word = word;
    return encoder_read();
}

int main() {
    EncoderConfig config{};
    config.sign = config.wheel_sign = 1;
    config.max_rpm = 600;
    config.max_rpm_verified = true;
    config.phase_reference_valid = true;
    config.wheel_cpr = 128;
    config.wheel_cpr_verified = true;
    config.wheel_velocity_window_us = 50000;
    encoder_init(config);

    auto r = read_at(110000, 0, response(4095));
    assert((r.flags & SAMPLE_VALID) && (r.wheel.flags & SAMPLE_VALID));
    assert(r.position12 == 4095 && r.position14 == 4095 * 4);
    assert(r.calibrated_alpha_rad < 0 && std::abs(r.calibrated_alpha_rad) < .002);
    assert(r.wheel.timestamp_us <= r.timestamp_us);
    assert(!(r.wheel.flags & PHASE_REFERENCE_VALID));

    // A parity fault cannot discard a real wheel count or prevent its history.
    r = read_at(115000, 1, 0);
    assert(!(r.flags & SAMPLE_VALID) && (r.flags & PARITY_ERROR));
    assert(!(r.flags & SPI_IO_ERROR) && r.raw_spi_word == 0 && r.position12 == -1);
    assert(std::isnan(r.calibrated_alpha_rad) && std::isnan(r.velocity_rad_s));
    assert(r.wheel.flags & SAMPLE_VALID);
    assert(r.wheel.raw_count == 1 && r.invalid_spi_responses == 1);
    r = read_at(120000, 2, 0x61ab);
    assert((r.flags & FORMAT_ERROR) && !(r.flags & PARITY_ERROR));
    assert(r.wheel.raw_count == 2);
    mock_spi_error = true;
    r = read_at(125000, 3, response(0));
    assert(r.flags & SPI_IO_ERROR);
    assert(r.wheel.raw_count == 3);
    mock_spi_error = false;
    mock_bus_error = true;
    const int reads_before_bus_failure = mock_spi_reads;
    r = read_at(130000, 4, response(0));
    assert((r.flags & SPI_IO_ERROR) && (r.wheel.flags & SAMPLE_VALID));
    assert(mock_spi_reads == reads_before_bus_failure && mock_cs_high);
    assert(r.position12 == -1 && std::isnan(r.calibrated_alpha_rad));
    mock_bus_error = false;
    for (int i = 5; i <= 10; ++i) r = read_at(110000 + i * 5000, i, response(i));
    assert((r.flags & SAMPLE_VALID) && (r.flags & VELOCITY_VALID));
    assert(r.wheel.flags & VELOCITY_VALID);
    assert(std::abs(r.wheel.velocity_rad_s - 10 * math::tau / 128 / .05) < 1e-10);

    // A pending overflow ISR invalidates only the snapshot, not the SPI data.
    mock_pending = 1;
    r = read_at(165000, 11, response(11));
    assert((r.flags & SAMPLE_VALID) && !(r.wheel.flags & SAMPLE_VALID));
    assert(std::isnan(r.wheel.angle_rad) && std::isnan(r.wheel.velocity_rad_s));
    mock_pending = 0;
    r = read_at(170000, 12, response(12));
    assert(r.wheel.raw_count == 12 && (r.wheel.flags & SAMPLE_VALID));
    assert(!(r.wheel.flags & VELOCITY_VALID));
    mock_counter_error = true;
    r = read_at(175000, 13, response(13));
    assert((r.flags & SAMPLE_VALID) && !(r.wheel.flags & SAMPLE_VALID));
    mock_counter_error = false;

    // New run resets derived histories, never cumulative wheel counts.
    encoder_begin_recording_segment();
    r = read_at(180000, 14, response(14));
    assert(r.wheel.raw_count == 14 && (r.flags & UNWRAP_CONTINUOUS));
    assert(!(r.flags & VELOCITY_VALID) && !(r.wheel.flags & VELOCITY_VALID));
    // Lost SPI intervals leave raw wrapped phase available; wheel still counts.
    r = read_at(240000, 25, response(15));
    assert((r.flags & SAMPLE_VALID) && (r.flags & CONTINUITY_LOST));
    assert(std::isnan(r.angle_rad) && std::isfinite(r.calibrated_alpha_rad));
    assert(r.wheel.raw_count == 25 && (r.wheel.flags & SAMPLE_VALID));
    // Snapshot gaps large enough to hide int32 rollover lose wheel continuity.
    r = read_at(27000000, 26, response(16));
    assert((r.flags & SAMPLE_VALID) && (r.wheel.flags & CONTINUITY_LOST));
    assert(!(r.wheel.flags & SAMPLE_VALID));
    encoder_begin_recording_segment();
    r = read_at(27005000, 27, response(17));
    assert(r.wheel.flags & CONTINUITY_LOST);

    std::cout << "Ordinary acquisition channel-isolation checks passed\n";
}

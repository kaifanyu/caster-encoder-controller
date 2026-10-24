#include "telemetry.h"
#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

int main() {
    caster::Sample samples[2]{};
    caster::Diagnostics diagnostics;
    for (int i = 0; i < 2; ++i) {
        auto& sample = samples[i];
        sample.sequence = std::numeric_limits<uint64_t>::max() - 1 + i;
        auto& r = sample.reading;
        r.timestamp_us = std::numeric_limits<int64_t>::max() - 1 + i;
        r.raw_count = i ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
        r.index_raw_count = r.raw_count;
        r.raw_spi_word = 65535;
        r.position14 = 16383;
        r.angle_rad = i ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::max();
        r.absolute_angle_rad = std::numeric_limits<double>::quiet_NaN();
        r.velocity_rad_s = -std::numeric_limits<double>::max();
        r.index_event_count = std::numeric_limits<uint64_t>::max();
        r.index_timestamp_us = r.timestamp_us;
        r.flags = caster::READY | caster::SAMPLE_VALID;
    }
    const char* boot = "0123456789abcdef";
    const char* device = "aa:bb:cc:dd:ee:01";
    char* samples_json = caster::serialize_samples(boot, device, samples, 2, diagnostics);
    char* metadata_json = caster::serialize_metadata(boot, device, caster::configuration(), 1);
    if (!samples_json || !metadata_json) return 1;
    puts(samples_json);
    puts(metadata_json);
    free(samples_json);
    free(metadata_json);
    return 0;
}

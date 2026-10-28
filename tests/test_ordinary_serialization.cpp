#include "config.h"
#include "telemetry.h"
#include "recorder_format.h"
#include "cJSON.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    using namespace caster;
    using namespace caster::recording;
    assert(argc == 2);
    Diagnostics diagnostics;
    const char* device = CASTER_NODE_ID == 5 ? (CASTER_WHEEL_INDEX == 1 ? "aa:bb:cc:dd:ee:11" : "aa:bb:cc:dd:ee:21") :
                         CASTER_NODE_ID == 6 ? (CASTER_WHEEL_INDEX == 1 ? "aa:bb:cc:dd:ee:12" : "aa:bb:cc:dd:ee:22") :
                         (CASTER_WHEEL_INDEX == 1 ? "aa:bb:cc:dd:ee:01" : "aa:bb:cc:dd:ee:02");
    Sample samples[3]{};
    for (int i = 0; i < 3; ++i) {
        auto& s = samples[i];
        s.sequence = static_cast<uint64_t>(i) + (1ULL << 53);
        auto& r = s.reading;
        r.timestamp_us = 1000000 + i * 5000;
        r.raw_spi_word = 0xc000;
        r.position12 = 0;
        r.absolute_angle_rad = r.calibrated_alpha_rad = r.angle_rad = 0;
        r.velocity_rad_s = r.filtered_velocity_rad_s = 0;
        r.flags = READY | SAMPLE_VALID | UNWRAP_CONTINUOUS | VELOCITY_VALID | RATE_BOUND_VERIFIED;
        r.wheel.timestamp_us = r.timestamp_us + 100;
        r.wheel.raw_count = INT64_MIN + i;
        r.wheel.angle_rad = -1.25;
        r.wheel.velocity_rad_s = r.wheel.filtered_velocity_rad_s = -.5;
        r.wheel.flags = READY | SAMPLE_VALID | UNWRAP_CONTINUOUS | VELOCITY_VALID | PPR_VERIFIED;
        if (i == 1) {
            r.flags = READY | PARITY_ERROR;
            r.raw_spi_word = 0;
            r.position12 = -1;
            r.absolute_angle_rad = r.calibrated_alpha_rad = r.angle_rad = missing_value;
            r.velocity_rad_s = r.filtered_velocity_rad_s = missing_value;
        } else if (i == 2) {
            r.wheel.flags = READY | CONTINUITY_LOST;
            r.wheel.angle_rad = r.wheel.velocity_rad_s = r.wheel.filtered_velocity_rad_s = missing_value;
        }
#if CASTER_NODE_ID == 5
        r.wheel = WheelReading{};
#elif CASTER_NODE_ID == 6
        auto wheel = r.wheel;
        r = Reading{};
        r.wheel = wheel;
        r.timestamp_us = wheel.timestamp_us;
#endif
        char* packet = serialize_samples("0123456789abcdef", device, &s, 1, diagnostics);
        assert(packet); puts(packet); free(packet);
    }
    char* metadata = serialize_metadata("0123456789abcdef", device, configuration(), 1);
    assert(metadata); puts(metadata);
    cJSON* header_json = cJSON_CreateObject();
    cJSON_AddStringToObject(header_json, "run_id", "ordinary-contract");
    cJSON_AddItemToObject(header_json, "packet", cJSON_Parse(metadata));
    cJSON_AddStringToObject(header_json, CASTER_NODE_ID == 6 ? "wheel_velocity_segment" : "swivel_unwrap_segment",
                           CASTER_NODE_ID == 6 ? "starts_at_recording; cumulative_count_preserved" :
                                                "starts_at_recording; no continuity across runs");
    char* text = cJSON_PrintUnformatted(header_json);
    std::vector<uint8_t> bytes(kHeaderBytes + 3 * kRecordBytes, 0);
    std::memcpy(bytes.data(), kMagic, 8);
    put(bytes.data() + 8, kVersion, 4); put(bytes.data() + 12, kRecordBytes, 4);
    put(bytes.data() + 16, std::strlen(text), 4);
    put(bytes.data() + 20, crc32(text, std::strlen(text)), 4);
    std::memcpy(bytes.data() + 24, text, std::strlen(text));
    for (int i = 0; i < 3; ++i) encode_record(samples[i], bytes.data() + kHeaderBytes + i * kRecordBytes);
    std::ofstream output(argv[1], std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    cJSON_Delete(header_json); free(text); free(metadata);
    return output ? 0 : 1;
}

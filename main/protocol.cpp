#include "telemetry.h"
#include "config.h"
#include "cJSON.h"
#include "esp_system.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>

namespace caster {
namespace {
void number(cJSON* obj, const char* key, double value) {
    if (std::isfinite(value)) {
        // Round-trip precision also keeps finite DBL_MAX finite at the receiver.
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.17g", value);
        cJSON_AddRawToObject(obj, key, buffer);
    } else cJSON_AddNullToObject(obj, key);
}
void count_string(cJSON* obj, const char* key, int64_t value) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%" PRId64, value);
    cJSON_AddStringToObject(obj, key, buffer);
}
void unsigned_integer(cJSON* obj, const char* key, uint64_t value) {
    // cJSON's double storage cannot represent every uint64_t. Format integer
    // tokens directly; raw counts remain strings for JavaScript consumers.
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
    cJSON_AddRawToObject(obj, key, buffer);
}
cJSON* envelope(const char* boot_id, const char* device_id, const char* type) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return nullptr;
    cJSON_AddNumberToObject(root, "v", 2);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "wheel_id", kWheel);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "node_id", CASTER_NODE_ID);
    cJSON_AddStringToObject(root, "boot_id", boot_id);
    return root;
}
char* finish(cJSON* root) {
    if (!root) return nullptr;
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}
}  // namespace

char* serialize_samples(const char* boot_id, const char* device_id, const Sample* samples, int count,
                        const Diagnostics& diagnostics) {
    cJSON* root = envelope(boot_id, device_id, "samples");
    if (!root) return nullptr;
    cJSON* array = cJSON_AddArrayToObject(root, "samples");
    if (!array) { cJSON_Delete(root); return nullptr; }
    for (int i = 0; i < count; ++i) {
        const auto& r = samples[i].reading;
        cJSON* obj = cJSON_CreateObject();
        if (!obj) { cJSON_Delete(root); return nullptr; }
        cJSON_AddItemToArray(array, obj);
        unsigned_integer(obj, "seq", samples[i].sequence);
        unsigned_integer(obj, "t_us", static_cast<uint64_t>(r.timestamp_us));
        if (CASTER_NODE_ID == 3) {
            cJSON_AddNullToObject(obj, "raw_count");
            number(obj, "raw_spi_word", r.raw_spi_word);
            if (r.position14 >= 0) number(obj, "position14", r.position14);
            else cJSON_AddNullToObject(obj, "position14");
            cJSON_AddNullToObject(obj, "index_raw_count");
        } else {
            if (r.flags & SAMPLE_VALID) count_string(obj, "raw_count", r.raw_count);
            else cJSON_AddNullToObject(obj, "raw_count");
            cJSON_AddNullToObject(obj, "raw_spi_word");
            cJSON_AddNullToObject(obj, "position14");
            count_string(obj, "index_raw_count", r.index_raw_count);
        }
        number(obj, "angle_rad", r.angle_rad);
        number(obj, "absolute_angle_rad", r.absolute_angle_rad);
        number(obj, "calibrated_alpha_rad", r.calibrated_alpha_rad);
        number(obj, "velocity_rad_s", r.velocity_rad_s);
        number(obj, "filtered_velocity_rad_s", r.filtered_velocity_rad_s);
        unsigned_integer(obj, "index_event_count", r.index_event_count);
        unsigned_integer(obj, "index_timestamp_us", static_cast<uint64_t>(r.index_timestamp_us));
        number(obj, "flags", r.flags);
    }
    cJSON* d = cJSON_AddObjectToObject(root, "diagnostics");
    if (!d) { cJSON_Delete(root); return nullptr; }
    number(d, "acquisition_overruns", diagnostics.acquisition_overruns.load());
    number(d, "queue_drops", diagnostics.queue_drops.load());
    number(d, "invalid_spi_responses", diagnostics.invalid_spi_responses.load());
    number(d, "index_queue_drops", diagnostics.index_queue_drops.load());
    number(d, "radio_failures", diagnostics.radio_failures.load());
    return finish(root);
}

char* serialize_metadata(const char* boot_id, const char* device_id, const EncoderConfig& c, int reset_reason) {
    cJSON* root = envelope(boot_id, device_id, "metadata");
    if (!root) return nullptr;
    cJSON_AddStringToObject(root, "firmware_version", kFirmwareVersion);
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(root, "role", kRole);
    cJSON_AddStringToObject(root, "encoder_type", c.role == 3 ? "AMT252B-V" : "AMT132S-V");
    cJSON_AddStringToObject(root, "transport", "wifi_udp");
    cJSON_AddStringToObject(root, "receiver_ipv4", RECEIVER_IPV4);
    number(root, "udp_port", UDP_PORT);
    cJSON_AddStringToObject(root, "configured_ipv4", kLocalIpv4);
    cJSON_AddStringToObject(root, "network_gateway", NETWORK_GATEWAY);
    cJSON_AddStringToObject(root, "network_netmask", NETWORK_NETMASK);
    number(root, "reset_reason", reset_reason);
    number(root, "sample_hz", SAMPLE_HZ);
    number(root, "transmit_hz", TRANSMIT_HZ);
    number(root, "coordinate_sign", c.sign);
    if (c.role == 3) {
        cJSON_AddNullToObject(root, "encoder_ppr");
        cJSON_AddNullToObject(root, "counts_per_rev");
    } else {
        number(root, "encoder_ppr", c.ppr);
        number(root, "counts_per_rev", static_cast<double>(c.ppr) * 4);
    }
    number(root, "quadrature_multiplier", c.role == 3 ? 0 : 4);
    cJSON_AddBoolToObject(root, "ppr_verified", c.role != 3 && c.ppr_verified);
    cJSON_AddBoolToObject(root, "phase_reference_valid", c.phase_reference_valid);
    cJSON_AddBoolToObject(root, "clock_sync_valid", false);
    cJSON_AddBoolToObject(root, "max_rpm_verified", c.max_rpm_verified);
    cJSON_AddBoolToObject(root, "continuity_across_reboot", false);
    number(root, "max_rpm", c.max_rpm);
    number(root, "glitch_filter_ns", c.glitch_filter_ns);
    count_string(root, "software_zero_count", c.zero_count);
    number(root, "alpha_zero_rad", c.alpha_zero_rad);
    number(root, "velocity_filter_tau_s", c.velocity_filter_tau_s);
    number(root, "readiness_delay_ms", 250);
    cJSON_AddStringToObject(root, "index_edge", "rising");
    cJSON_AddStringToObject(root, "index_timing", "GPIO ISR observation; latency not bench measured");
    cJSON_AddStringToObject(root, "storage", "explicit local flash runs; HTTP verified download; UDP live outside runs");
    cJSON_AddNumberToObject(root, "recorder_http_port", 8080);
    cJSON_AddNumberToObject(root, "recorder_discovery_port", 5010);
    cJSON_AddStringToObject(root, "gpio", c.role == 3 ? "CS=21,SCLK=19,MISO=20,MOSI=18" : "A=0,B=1,Z=2");
    return finish(root);
}
}  // namespace caster

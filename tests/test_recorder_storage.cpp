// Actual firmware recorder with deterministic RTOS/network/flash substitutes.
// Tests exercise handlers, capture barriers and recovery, not a duplicate model.
#include "../main/recorder.cpp"
#include <cassert>
#include <iostream>
#include <limits>

namespace caster {
const char* transport_device_id() { return "aa:bb:cc:dd:ee:01"; }
char* serialize_metadata(const char* boot, const char* device, const EncoderConfig&, int) {
    cJSON* object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "boot_id", boot);
    cJSON_AddStringToObject(object, "device_id", device);
    cJSON_AddStringToObject(object, "wheel_id", kWheel);
    cJSON_AddNumberToObject(object, "node_id", CASTER_NODE_ID);
    return finish_json(object);
}
}
using namespace caster;
using namespace caster::recording;
Diagnostics test_counters;

void boot(bool clean = false, const char* boot_name = "0123456789abcdef") {
    if (clean) {
        mock_flash.assign(mock_partition.size, 0xff);
        mock_prepare_intent.clear();
        mock_erase_calls = 0;
    }
    delete write_queue; write_queue = nullptr;
    state = State::idle; partition = nullptr;
    current_boot[0] = log_boot[0] = run_id[0] = reason[0] = 0;
    count = accepted = rolling_crc = capacity = 0;
    start_us = stop_us = -1;
    header_valid = acknowledged = stopping = stop_seen = false;
    journal_ready = prepare_incomplete = false;
    diagnostics_known = true;
    tracking.store(false); new_segment.store(false); suppress_live.store(false);
    std::memset(final_diagnostics, 0, sizeof(final_diagnostics));
    mock_write_budget = -1;
    mock_on_delay = nullptr;
    recorder_init(boot_name, 1, &test_counters);
}
Sample sample(uint64_t sequence, int64_t timestamp) {
    Sample value{};
    value.sequence = sequence;
    value.reading.timestamp_us = timestamp;
    value.reading.raw_count = -123456789012345678LL;
    value.reading.position14 = -1;
    value.reading.flags = READY | SAMPLE_VALID;
    value.reading.angle_rad = 1.25;
    value.reading.velocity_rad_s = -2.5;
    value.reading.index_raw_count = INT64_MIN;
    return value;
}
httpd_req_t api(const char* path, const std::string& body) {
    httpd_req_t req;
    req.uri = path; req.body = body; req.content_len = body.size();
    assert(command(&req) == ESP_OK);
    return req;
}
void begin(const char* id = "run-1") {
    assert(prepare(id));
    assert(start(id));
    assert(recorder_new_segment());
}
void end(int64_t timestamp) {
    request_stop("requested", timestamp, false);
    recorder_capture(sample(99999, timestamp + 1));
    while (stopping) writer_pass();
    assert(state == State::stopped);
}
void format_test() {
    assert(crc32("123456789", 9) == 0xcbf43926);
    assert(crc32("56789", 5, crc32("1234", 4)) == 0xcbf43926);
    uint8_t bytes[kRecordBytes];
    auto value = sample(UINT64_MAX, INT64_MAX);
    encode_record(value, bytes);
    assert(get(bytes, 8) == UINT64_MAX);
    assert(get(bytes + 8, 8) == INT64_MAX);
    assert(static_cast<int64_t>(get(bytes + 16, 8)) == value.reading.raw_count);
    assert(get(bytes + 26, 2) == 65535);
    assert(get(bytes + 32, 8) == 0x3ff4000000000000ULL);
    assert(valid_record(bytes));
    bytes[20] ^= 1;
    assert(!valid_record(bytes));
    std::memset(bytes, 0xff, sizeof(bytes));
    assert(!valid_record(bytes));
}
void lifecycle_test() {
    boot(true);
    assert(state == State::idle);
    recorder_capture(sample(0, mock_time)); writer_pass();
    assert(count == 0 && erased(mock_flash.data(), mock_flash.size()));
    assert(!valid_run_id("_bad") && !valid_run_id("bad/path") && valid_run_id("a.b-1_2"));
    assert(!valid_run_id(std::string(65, 'a').c_str()));
    assert(api("/api/prepare", "{\"run_id\":\"run-1\"}").status == "200 OK");
    assert(state == State::prepared && mock_prepare_intent.empty());
    const int erases = mock_erase_calls;
    assert(prepare("run-1") && mock_erase_calls == erases);
    assert(!prepare("unread-replacement"));
    assert(start("run-1"));
    const int64_t began = start_us;
    mock_time += 100;
    assert(start("run-1") && start_us == began);
    // A read already in progress when start arrives must not use the old segment.
    recorder_capture(sample(1, began + 1));
    assert(accepted == 0);
    assert(recorder_new_segment());
    recorder_capture(sample(2, began - 1));
    recorder_capture(sample(3, began + 5000));
    assert(accepted == 1 && count == 0);
    // Stop must include a pre-boundary acquisition finishing after the command.
    request_stop("requested", began + 10000, false);
    writer_pass();
    assert(stopping && state == State::recording); // waiting for capture barrier
    recorder_capture(sample(4, began + 9000));
    recorder_capture(sample(5, began + 10001));
    while (stopping) writer_pass();
    assert(count == 2 && state == State::stopped && std::strcmp(reason, "requested") == 0);
    assert(!tracking.load() && !recorder_suppress_live());
    assert(start("run-1") && state == State::stopped && start_us == began);
    assert(api("/api/stop", "{\"run_id\":\"run-1\"}").status == "200 OK");
    assert(!prepare("run-2"));
    auto bad_ack = api("/api/ack", "{\"run_id\":\"run-1\",\"crc32\":0}");
    assert(bad_ack.status == "409 Conflict" && !acknowledged);
    httpd_req_t log;
    log.uri = "/api/log?run_id=run-1";
    assert(download(&log) == ESP_OK);
    assert(log.response.size() == kHeaderBytes + 2 * kRecordBytes);
    const auto* bytes = reinterpret_cast<const uint8_t*>(log.response.data());
    assert(std::memcmp(bytes, "CASTLOG1", 8) == 0 && get(bytes + 12, 4) == 100);
    assert(crc32(bytes + kHeaderBytes, 200) == rolling_crc);
    const uint32_t saved_crc = rolling_crc;
    boot(false, "fedcba9876543210");
    assert(count == 2 && rolling_crc == saved_crc && state == State::stopped && diagnostics_known);
    assert(std::strcmp(log_boot, "0123456789abcdef") == 0 && std::strcmp(current_boot, log_boot) != 0);
    assert(!prepare("run-2"));
    auto ack = api("/api/ack", "{\"run_id\":\"run-1\",\"crc32\":" + std::to_string(saved_crc) + "}");
    assert(ack.status == "200 OK" && acknowledged);
    const auto preserved = mock_flash;
    boot();
    assert(acknowledged && mock_flash == preserved);
    assert(prepare("run-2") && state == State::prepared);
    assert(mock_erase_calls == erases + 1);
}
void interrupted_test() {
    boot(true); begin();
    recorder_capture(sample(1, start_us + 1));
    recorder_capture(sample(2, start_us + 2));
    recorder_capture(sample(3, start_us + 3));
    // Power fails after two full records plus part of the third. Emulate the
    // write, but don't run the failure/finalize code before reboot.
    uint8_t bytes[300];
    for (size_t i = 0; i < 3; ++i) assert(xQueueReceive(write_queue, bytes + i * 100, 0));
    mock_write_budget = 245;
    assert(esp_partition_write(partition, kHeaderBytes, bytes, sizeof(bytes)) != ESP_OK);
    boot();
    assert(count == 2 && state == State::stopped && !diagnostics_known && !acknowledged);
    assert(std::strcmp(reason, "interrupted") == 0 && !prepare("next"));
    assert(!tracking.load());
    // Corrupt the middle of an otherwise valid prefix: salvage only before it.
    mock_flash[kHeaderBytes + kRecordBytes + 20] ^= 1;
    boot();
    assert(count == 1 && !acknowledged && !diagnostics_known);
}
void prepare_interruption_test() {
    boot(true);
    mock_write_budget = 100; // header cannot finish
    assert(!prepare("half-header") && prepare_incomplete);
    assert(!mock_prepare_intent.empty());
    const auto preserved = mock_flash;
    boot();
    assert(state == State::idle && prepare_incomplete && !tracking.load());
    assert(std::strcmp(reason, "prepare_interrupted") == 0 && mock_flash == preserved);
    assert(prepare("retry") && mock_prepare_intent.empty());
    // Unknown corruption is never treated as an interrupted authorized prepare.
    mock_flash[0] ^= 1;
    boot();
    assert(state == State::error && !header_valid && !prepare("erase-corruption"));
}
void capacity_and_overflow_test() {
    boot(true); begin();
    const int64_t began = start_us;
    const uint32_t max_samples = capacity;
    for (uint32_t i = 0; i < max_samples; ++i) {
        recorder_capture(sample(i, began + i + 1));
        writer_pass();
    }
    assert(count == max_samples && state == State::stopped && std::strcmp(reason, "capacity") == 0);
    const auto full = mock_flash;
    recorder_capture(sample(max_samples, began + max_samples + 1)); writer_pass();
    assert(mock_flash == full);
    boot(true); begin();
    write_queue->capacity = 2;
    recorder_capture(sample(1, start_us + 1));
    recorder_capture(sample(2, start_us + 2));
    recorder_capture(sample(3, start_us + 3));
    writer_pass();
    assert(count == 2 && state == State::stopped && std::strcmp(reason, "buffer_overflow") == 0);
}
void api_stop_test() {
    boot(true); begin("api.run");
    recorder_capture(sample(1, start_us + 1));
    mock_time += 100;
    mock_on_delay = [] {
        recorder_capture(sample(2, mock_time));
        writer_pass();
    };
    const auto response = api("/api/stop", "{\"run_id\":\"api.run\"}");
    assert(response.status == "200 OK" && state == State::stopped && count == 1);
    cJSON* status = cJSON_Parse(response.response.c_str());
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(status, "finalizing")));
    assert(cJSON_GetObjectItemCaseSensitive(status, "data_bytes")->valuedouble == 4196);
    cJSON_Delete(status);
    httpd_req_t sync;
    assert(get_sync(&sync) == ESP_OK);
    status = cJSON_Parse(sync.response.c_str());
    assert(cJSON_GetObjectItemCaseSensitive(status, "rx_us")->valuedouble == mock_time);
    assert(cJSON_GetObjectItemCaseSensitive(status, "tx_us")->valuedouble == mock_time);
    cJSON_Delete(status);
}
int main() {
    format_test(); lifecycle_test(); interrupted_test(); prepare_interruption_test();
    capacity_and_overflow_test(); api_stop_test();
    delete write_queue;
    std::cout << "recorder format, API, lifecycle, capture barrier, torn writes, unread protection, capacity: passed\n";
}

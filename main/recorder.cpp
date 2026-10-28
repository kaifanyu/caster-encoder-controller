#include "recorder.h"
#include "recorder_format.h"
#include "config.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace caster {
namespace {
using namespace recording;
constexpr const char* kTag = "recorder";
constexpr int kHttpPort = 8080;
constexpr int kDiscoveryPort = 5010;
constexpr size_t kQueueRecords = 256;
constexpr size_t kBatchRecords = 16;
enum class State { idle, prepared, recording, stopped, error };
State state = State::idle;
const esp_partition_t* partition = nullptr;
SemaphoreHandle_t lock;
QueueHandle_t write_queue;
Diagnostics* counters;
char current_boot[17]{};
char log_boot[17]{};
char run_id[81]{};
char reason[32]{};
int reset_reason;
uint32_t count = 0, accepted = 0, rolling_crc = 0, capacity = 0;
uint32_t final_diagnostics[5]{};
int64_t start_us = -1, stop_us = -1;
bool header_valid = false, acknowledged = false, stopping = false, stop_seen = false;
bool diagnostics_known = true;
nvs_handle_t prepare_journal = 0;
bool journal_ready = false, prepare_incomplete = false;
std::atomic<bool> tracking{false}, new_segment{false}, suppress_live{false};

struct Guard {
    Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(lock); }
};
const char* state_name() {
    switch (state) {
        case State::idle: return "idle";
        case State::prepared: return "prepared";
        case State::recording: return "recording";
        case State::stopped: return "stopped";
        default: return "error";
    }
}
void snapshot_diagnostics(uint32_t* destination) {
    destination[0] = counters->acquisition_overruns.load();
    destination[1] = counters->queue_drops.load();
    destination[2] = counters->invalid_spi_responses.load();
    destination[3] = counters->index_queue_drops.load();
    destination[4] = counters->radio_failures.load();
}
void integer(cJSON* object, const char* key, int64_t value) {
    char text[32];
    snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
    cJSON_AddRawToObject(object, key, text);
}
cJSON* identity() {
    cJSON* object = cJSON_CreateObject();
    if (!object) return nullptr;
    cJSON_AddNumberToObject(object, "v", 1);
    cJSON_AddStringToObject(object, "wheel_id", kWheel);
    cJSON_AddStringToObject(object, "device_id", transport_device_id());
    cJSON_AddNumberToObject(object, "node_id", CASTER_NODE_ID);
#if CASTER_NODE_ID >= 4
    cJSON_AddStringToObject(object, "caster_type", "ordinary");
    cJSON_AddNumberToObject(object, "record_version", kVersion);
    cJSON_AddNumberToObject(object, "record_bytes", kRecordBytes);
#endif
    cJSON_AddStringToObject(object, "boot_id", current_boot);
    return object;
}
char* finish_json(cJSON* object) {
    if (!object) return nullptr;
    char* result = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    return result;
}
char* status_json() {
    Guard guard;
    cJSON* object = identity();
    if (!object) return nullptr;
    cJSON_AddStringToObject(object, "state", state_name());
    if (run_id[0]) cJSON_AddStringToObject(object, "run_id", run_id);
    else cJSON_AddNullToObject(object, "run_id");
    if (log_boot[0]) cJSON_AddStringToObject(object, "log_boot_id", log_boot);
    else cJSON_AddNullToObject(object, "log_boot_id");
    integer(object, "sample_count", count);
    integer(object, "data_bytes", header_valid ? kHeaderBytes + count * kRecordBytes : 0);
    integer(object, "crc32", rolling_crc);
    integer(object, "capacity_samples", capacity);
    if (start_us >= 0) integer(object, "start_us", start_us);
    else cJSON_AddNullToObject(object, "start_us");
    if (stop_us >= 0) integer(object, "stop_us", stop_us);
    else cJSON_AddNullToObject(object, "stop_us");
    cJSON_AddStringToObject(object, "stop_reason", reason);
    cJSON_AddBoolToObject(object, "acknowledged", acknowledged);
    cJSON_AddBoolToObject(object, "diagnostics_known", diagnostics_known);
    cJSON_AddBoolToObject(object, "finalizing", stopping);
    cJSON_AddBoolToObject(object, "supports_discard_existing", true);
    cJSON_AddNumberToObject(object, "http_port", kHttpPort);
    cJSON_AddNumberToObject(object, "sample_hz", SAMPLE_HZ);
    cJSON* diagnostics = cJSON_AddObjectToObject(object, "diagnostics");
    uint32_t values[5];
    if (state == State::stopped || state == State::error)
        std::memcpy(values, final_diagnostics, sizeof(values));
    else snapshot_diagnostics(values);
    const char* names[] = {"acquisition_overruns", "queue_drops", "invalid_spi_responses",
                           "index_queue_drops", "radio_failures"};
    for (size_t i = 0; i < 5; ++i) integer(diagnostics, names[i], values[i]);
    return finish_json(object);
}
size_t footer_offset() { return partition->size - kFooterBytes; }
bool read_control(size_t offset, uint8_t* block, const char* magic) {
    return esp_partition_read(partition, footer_offset() + offset, block, kControlBytes) == ESP_OK &&
           valid_control(block, magic);
}
bool write_control(size_t offset, uint8_t* block) {
    seal_control(block);
    return esp_partition_write(partition, footer_offset() + offset, block, kControlBytes) == ESP_OK;
}
bool valid_run_id(const char* value) {
    if (!value || !value[0] || std::strlen(value) > 64) return false;
    const auto alphanumeric = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alphanumeric(value[0])) return false;
    for (const char* p = value; *p; ++p)
        if (!(alphanumeric(*p) || *p == '-' || *p == '_' || *p == '.')) return false;
    return true;
}
// Recover only the contiguous valid prefix. A torn final write cannot create
// invented samples; a valid record after a corrupt record is deliberately ignored.
bool scan_records() {
    uint8_t batch[kBatchRecords * kRecordBytes];
    count = rolling_crc = 0;
    int64_t previous_time = -1;
    uint64_t previous_sequence = 0;
    while (count < capacity) {
        const size_t n = capacity - count < kBatchRecords ? capacity - count : kBatchRecords;
        if (esp_partition_read(partition, kHeaderBytes + count * kRecordBytes,
                               batch, n * kRecordBytes) != ESP_OK) return false;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* record = batch + i * kRecordBytes;
            const auto timestamp = static_cast<int64_t>(get(record + 8, 8));
            const uint64_t sequence = get(record, 8);
            if (!valid_record(record) || timestamp < start_us || timestamp <= previous_time ||
                (count && sequence <= previous_sequence)) return true;
            rolling_crc = crc32(record, kRecordBytes, rolling_crc);
            previous_time = timestamp;
            previous_sequence = sequence;
            ++count;
        }
    }
    return true;
}
void recover() {
    // Intent is committed before erase, and removed before start is permitted.
    // Thus a pending intent proves that any abandoned replacement was never
    // recording. It is safe to retry prepare, without auto-erasing forensic data.
    char pending[81];
    size_t pending_size = sizeof(pending);
    const esp_err_t pending_result = nvs_get_str(prepare_journal, "prepare", pending, &pending_size);
    if (pending_result == ESP_OK && valid_run_id(pending)) {
        prepare_incomplete = true;
        std::strcpy(reason, "prepare_interrupted");
        return;
    }
    if (pending_result != ESP_ERR_NVS_NOT_FOUND) {
        journal_ready = false;
        state = State::error; std::strcpy(reason, "journal_read_error"); return;
    }
    auto* header = static_cast<uint8_t*>(malloc(kHeaderBytes));
    if (!header || esp_partition_read(partition, 0, header, kHeaderBytes) != ESP_OK) {
        free(header); state = State::error; std::strcpy(reason, "read_error"); return;
    }
    if (erased(header, kHeaderBytes)) { free(header); return; }
    const size_t length = get(header + 16, 4);
    const bool valid = std::memcmp(header, kMagic, 8) == 0 &&
        get(header + 8, 4) == kVersion && get(header + 12, 4) == kRecordBytes &&
        length > 0 && length < kHeaderBytes - 24 &&
        get(header + 20, 4) == crc32(header + 24, length);
    if (!valid) {
        free(header); state = State::error; std::strcpy(reason, "invalid_header"); return;
    }
    header[24 + length] = 0;
    cJSON* metadata = cJSON_Parse(reinterpret_cast<char*>(header + 24));
    free(header);
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(metadata, "run_id");
    const cJSON* packet = cJSON_GetObjectItemCaseSensitive(metadata, "packet");
    const cJSON* boot = cJSON_GetObjectItemCaseSensitive(packet, "boot_id");
    if (!cJSON_IsString(id) || !valid_run_id(id->valuestring) || !cJSON_IsString(boot) ||
        std::strlen(boot->valuestring) != 16) {
        cJSON_Delete(metadata); state = State::error; std::strcpy(reason, "invalid_metadata"); return;
    }
    std::strcpy(run_id, id->valuestring);
    std::strcpy(log_boot, boot->valuestring);
    const cJSON* wheel = cJSON_GetObjectItemCaseSensitive(packet, "wheel_id");
    const cJSON* device = cJSON_GetObjectItemCaseSensitive(packet, "device_id");
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(packet, "node_id");
    const bool identity_matches = cJSON_IsString(wheel) && cJSON_IsString(device) && cJSON_IsNumber(node) &&
        std::strcmp(wheel->valuestring, kWheel) == 0 &&
        std::strcmp(device->valuestring, transport_device_id()) == 0 && node->valuedouble == CASTER_NODE_ID;
    cJSON_Delete(metadata);
    header_valid = true;
    uint8_t control[kControlBytes];
    const bool ready = read_control(kReadyOffset, control, "CASTRDY1");
    if (read_control(kStartOffset, control, "CASTBEG1"))
        start_us = static_cast<int64_t>(get(control + 8, 8));
    if (!scan_records()) {
        state = State::error; std::strcpy(reason, "read_error"); return;
    }
    accepted = count;
    state = State::stopped;
    diagnostics_known = false;
    std::strcpy(reason, ready ? "interrupted" : "prepare_interrupted");
    if (count) {
        uint8_t last[kRecordBytes];
        if (esp_partition_read(partition, kHeaderBytes + (count - 1) * kRecordBytes,
                               last, sizeof(last)) == ESP_OK)
            stop_us = static_cast<int64_t>(get(last + 8, 8));
    }
    if (read_control(kStopOffset, control, "CASTEND1") &&
        get(control + 8, 4) == count && get(control + 12, 4) == rolling_crc) {
        start_us = static_cast<int64_t>(get(control + 16, 8));
        stop_us = static_cast<int64_t>(get(control + 24, 8));
        std::memcpy(reason, control + 32, sizeof(reason));
        reason[sizeof(reason) - 1] = 0;
        for (size_t i = 0; i < 5; ++i) final_diagnostics[i] = get(control + 64 + 4 * i, 4);
        diagnostics_known = true;
    }
    if (read_control(kAckOffset, control, "CASTACK1") &&
        get(control + 8, 4) == count && get(control + 12, 4) == rolling_crc)
        acknowledged = true;
    if (!identity_matches) { state = State::error; std::strcpy(reason, "identity_mismatch"); }
}
// Caller owns lock; no recording is active during erase/header construction.
bool prepare(const char* requested, bool discard_existing = false) {
    if (!partition || !journal_ready) return false;
    // Even an explicit discard cannot race an acquisition or writer draining
    // outside this lock. Stop and finish that recording before replacing it.
    if (discard_existing && (state == State::recording || tracking.load() || stopping)) return false;
    if (run_id[0] && std::strcmp(requested, run_id) == 0) return header_valid;
    if (state == State::recording || tracking.load() || stopping) return false;
    if (!discard_existing && state != State::idle && !acknowledged && !prepare_incomplete) return false;
    suppress_live.store(true);
    if (nvs_set_str(prepare_journal, "prepare", requested) != ESP_OK ||
        nvs_commit(prepare_journal) != ESP_OK) {
        state = State::error; std::strcpy(reason, "journal_write_error"); return false;
    }
    prepare_incomplete = true;
    header_valid = false;
    if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK) {
        state = State::error; std::strcpy(reason, "erase_error"); return false;
    }
    cJSON* metadata = cJSON_CreateObject();
    char* packet_text = serialize_metadata(current_boot, transport_device_id(), configuration(), reset_reason);
    cJSON* packet = packet_text ? cJSON_Parse(packet_text) : nullptr;
    free(packet_text);
    if (!metadata || !packet) {
        cJSON_Delete(metadata); cJSON_Delete(packet); state = State::error;
        std::strcpy(reason, "allocation_error"); return false;
    }
    cJSON_AddStringToObject(metadata, "run_id", requested);
    cJSON_AddItemToObject(metadata, "packet", packet);
    cJSON_AddStringToObject(metadata, CASTER_NODE_ID == 6 ? "wheel_velocity_segment" :
                           CASTER_NODE_ID >= 4 ? "swivel_unwrap_segment" : "center_unwrap_segment",
                           CASTER_NODE_ID == 6 ? "velocity baseline resets at recording; wheel count remains cumulative for this boot" :
                           "starts_at_recording; no continuity across runs");
    char* text = finish_json(metadata);
    const size_t length = text ? std::strlen(text) : kHeaderBytes;
    auto* header = static_cast<uint8_t*>(calloc(1, kHeaderBytes));
    if (!header || length >= kHeaderBytes - 24) {
        free(text); free(header); state = State::error; std::strcpy(reason, "metadata_too_large"); return false;
    }
    std::memcpy(header, kMagic, 8);
    put(header + 8, kVersion, 4); put(header + 12, kRecordBytes, 4);
    put(header + 16, length, 4); put(header + 20, crc32(text, length), 4);
    std::memcpy(header + 24, text, length);
    const bool ok = esp_partition_write(partition, 0, header, kHeaderBytes) == ESP_OK;
    free(text); free(header);
    if (!ok) { state = State::error; std::strcpy(reason, "header_write_error"); return false; }
    uint8_t ready[kControlBytes]{};
    std::memcpy(ready, "CASTRDY1", 8);
    if (!write_control(kReadyOffset, ready)) {
        state = State::error; std::strcpy(reason, "ready_write_error"); return false;
    }
    if (nvs_erase_key(prepare_journal, "prepare") != ESP_OK || nvs_commit(prepare_journal) != ESP_OK) {
        state = State::error; std::strcpy(reason, "journal_commit_error"); return false;
    }
    prepare_incomplete = false;
    xQueueReset(write_queue);
    std::strcpy(run_id, requested); std::strcpy(log_boot, current_boot);
    count = accepted = rolling_crc = 0;
    start_us = stop_us = -1;
    std::memset(final_diagnostics, 0, sizeof(final_diagnostics));
    diagnostics_known = true;
    reason[0] = 0;
    acknowledged = stopping = stop_seen = false;
    header_valid = true;
    state = State::prepared;
    return true;
}
bool start(const char* requested) {
    if (!header_valid || std::strcmp(run_id, requested) != 0) return false;
    // Retry never restarts an already terminal recording.
    if (state == State::recording || state == State::stopped) return true;
    if (state != State::prepared || std::strcmp(log_boot, current_boot) != 0) return false;
    start_us = esp_timer_get_time();
    uint8_t block[kControlBytes]{};
    std::memcpy(block, "CASTBEG1", 8); put(block + 8, start_us, 8);
    if (!write_control(kStartOffset, block)) {
        state = State::error; std::strcpy(reason, "start_write_error"); return false;
    }
    state = State::recording;
    new_segment.store(true);
    tracking.store(true);
    suppress_live.store(true);
    return true;
}
void request_stop(const char* why, int64_t timestamp, bool seen) {
    if (stopping) return;
    stop_us = timestamp;
    std::snprintf(reason, sizeof(reason), "%s", why);
    stopping = true;
    stop_seen = seen;
}
// Writer only, after capture barrier and queue drain. Lock held.
void finalize() {
    tracking.store(false);
    snapshot_diagnostics(final_diagnostics);
    diagnostics_known = true;
    uint8_t block[kControlBytes]{};
    std::memcpy(block, "CASTEND1", 8);
    put(block + 8, count, 4); put(block + 12, rolling_crc, 4);
    put(block + 16, static_cast<uint64_t>(start_us), 8);
    put(block + 24, static_cast<uint64_t>(stop_us), 8);
    std::memcpy(block + 32, reason, sizeof(reason));
    for (size_t i = 0; i < 5; ++i) put(block + 64 + 4 * i, final_diagnostics[i], 4);
    state = write_control(kStopOffset, block) ? State::stopped : State::error;
    if (state == State::error) std::strcpy(reason, "footer_write_error");
    stopping = false;
    suppress_live.store(false);
    ESP_LOGI(kTag, "run=%s samples=%lu crc32=%lu reason=%s", run_id,
             static_cast<unsigned long>(count), static_cast<unsigned long>(rolling_crc), reason);
}
void writer_pass() {
    uint8_t batch[kBatchRecords * kRecordBytes];
        size_t n = 0;
        if (xQueueReceive(write_queue, batch, pdMS_TO_TICKS(10)) == pdTRUE) {
            n = 1;
            // Collect up to 16 rows (~80 ms at 200 Hz) rather than program flash
            // on every sample. A stopped stream drains after a 20 ms quiet gap.
            while (n < kBatchRecords && xQueueReceive(write_queue, batch + n * kRecordBytes,
                                                     pdMS_TO_TICKS(20)) == pdTRUE) ++n;
        }
        if (n) {
            uint32_t offset_count;
            { Guard guard; offset_count = count; }
            const bool ok = esp_partition_write(partition, kHeaderBytes + offset_count * kRecordBytes,
                                                batch, n * kRecordBytes) == ESP_OK;
            Guard guard;
            if (ok) {
                rolling_crc = crc32(batch, n * kRecordBytes, rolling_crc);
                count += n;
            } else {
                tracking.store(false);
                xQueueReset(write_queue);
                scan_records();
                request_stop("storage_error", esp_timer_get_time(), true);
                std::strcpy(reason, "storage_error");
                stop_seen = true;
            }
        }
        {
            Guard guard;
            if (stopping && stop_seen && uxQueueMessagesWaiting(write_queue) == 0) finalize();
        }
}
void writer(void*) { while (true) writer_pass(); }

esp_err_t send_json(httpd_req_t* request, char* text) {
    if (!text) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "allocation failed");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return result;
}
esp_err_t conflict(httpd_req_t* request, const char* message) {
    httpd_resp_set_status(request, "409 Conflict");
    cJSON* object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "error", message);
    return send_json(request, finish_json(object));
}
cJSON* request_json(httpd_req_t* request) {
    // Two 64-character run IDs plus the explicit discard identity/boot guards.
    if (request->content_len <= 0 || request->content_len >= 512) return nullptr;
    char buffer[512];
    size_t used = 0;
    while (used < request->content_len) {
        const int got = httpd_req_recv(request, buffer + used, request->content_len - used);
        if (got <= 0) return nullptr;
        used += got;
    }
    buffer[used] = 0;
    cJSON* object = cJSON_ParseWithOpts(buffer, nullptr, true);
    if (!cJSON_IsObject(object)) { cJSON_Delete(object); return nullptr; }
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(object, "run_id");
    if (!cJSON_IsString(id) || !valid_run_id(id->valuestring)) { cJSON_Delete(object); return nullptr; }
    return object;
}
esp_err_t get_status(httpd_req_t* request) { return send_json(request, status_json()); }
esp_err_t get_sync(httpd_req_t* request) {
    const int64_t rx = esp_timer_get_time();
    cJSON* object = identity();
    integer(object, "rx_us", rx);
    integer(object, "tx_us", esp_timer_get_time());
    return send_json(request, finish_json(object));
}
esp_err_t command(httpd_req_t* request) {
    cJSON* body = request_json(request);
    if (!body) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "run_id: 1..64 letters/digits/./-/_; first character alphanumeric");
    const char* id = cJSON_GetObjectItemCaseSensitive(body, "run_id")->valuestring;
    const bool is_prepare = std::strcmp(request->uri, "/api/prepare") == 0;
    const bool is_stop = std::strcmp(request->uri, "/api/stop") == 0;
    const cJSON* discard = cJSON_GetObjectItemCaseSensitive(body, "discard_existing");
    const cJSON* expected_device = cJSON_GetObjectItemCaseSensitive(body, "expected_device_id");
    const cJSON* expected_boot = cJSON_GetObjectItemCaseSensitive(body, "expected_boot_id");
    const cJSON* expected_run = cJSON_GetObjectItemCaseSensitive(body, "expected_run_id");
    const bool discard_existing = cJSON_IsTrue(discard);
    const bool guarded_stop = is_stop && (expected_device || expected_boot);
    if (is_prepare && ((discard && !cJSON_IsBool(discard)) ||
        (discard_existing && (!cJSON_IsString(expected_device) || !cJSON_IsString(expected_boot) ||
            !(cJSON_IsNull(expected_run) ||
              (cJSON_IsString(expected_run) && valid_run_id(expected_run->valuestring))))))) {
        cJSON_Delete(body);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
            "discard_existing must be boolean; true requires expected_device_id, expected_boot_id, expected_run_id (string or null)");
    }
    if (guarded_stop && (!cJSON_IsString(expected_device) || !cJSON_IsString(expected_boot))) {
        cJSON_Delete(body);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
            "guarded stop requires both expected_device_id and expected_boot_id strings");
    }
    bool ok = false, wait_stop = false;
    const char* conflict_message = "state/run/CRC conflict; unread recordings cannot be overwritten";
    {
        Guard guard;
        if (is_prepare) {
            const bool same_run = run_id[0] && std::strcmp(id, run_id) == 0;
            // Device and boot must match even for a retry. Compare the previous
            // run only for a replacement: an identical successful request still
            // carries the old run guard and must never erase its new recording.
            if (discard_existing &&
                (std::strcmp(expected_device->valuestring, transport_device_id()) != 0 ||
                 std::strcmp(expected_boot->valuestring, current_boot) != 0))
                conflict_message = "discard identity/boot conflict; refresh status";
            else if (discard_existing && !same_run &&
                     (run_id[0] ? !cJSON_IsString(expected_run) || std::strcmp(expected_run->valuestring, run_id) != 0
                                : !cJSON_IsNull(expected_run)))
                conflict_message = "discard retained-run conflict; refresh status";
            else {
                ok = prepare(id, discard_existing);
                if (discard_existing && !ok)
                    conflict_message = "discard requires usable storage and an inactive recorder; stop and finalize before replacing";
            }
        }
        else if (std::strcmp(request->uri, "/api/start") == 0) ok = start(id);
        else if (is_stop) {
            // Guard the stop preceding a discard too: reassigned IPs or a
            // reboot must not stop a different physical/current-boot session.
            if (guarded_stop &&
                (std::strcmp(expected_device->valuestring, transport_device_id()) != 0 ||
                 std::strcmp(expected_boot->valuestring, current_boot) != 0))
                conflict_message = "stop identity/boot conflict; refresh status";
            else if (header_valid && std::strcmp(run_id, id) == 0) {
                ok = state == State::stopped || state == State::error;
                if (state == State::recording || state == State::prepared) {
                    request_stop(state == State::prepared ? "cancelled" : "requested",
                                 esp_timer_get_time(), state == State::prepared);
                    ok = wait_stop = true;
                }
            }
        } else if (std::strcmp(request->uri, "/api/ack") == 0) {
            const cJSON* crc = cJSON_GetObjectItemCaseSensitive(body, "crc32");
            if (header_valid && std::strcmp(id, run_id) == 0 &&
                (state == State::stopped || state == State::error) && !stopping &&
                cJSON_IsNumber(crc) && std::isfinite(crc->valuedouble) &&
                crc->valuedouble >= 0 && crc->valuedouble <= UINT32_MAX &&
                crc->valuedouble == rolling_crc) {
                if (acknowledged) ok = true;
                else {
                    uint8_t block[kControlBytes]{};
                    std::memcpy(block, "CASTACK1", 8);
                    put(block + 8, count, 4); put(block + 12, rolling_crc, 4);
                    ok = acknowledged = write_control(kAckOffset, block);
                }
            }
        }
    }
    cJSON_Delete(body);
    if (!ok) return conflict(request, conflict_message);
    if (wait_stop) {
        const int64_t deadline = esp_timer_get_time() + 30000000;
        while (true) {
            { Guard guard; if (!stopping) break; }
            if (esp_timer_get_time() >= deadline)
                return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "finalization incomplete; retry stop/status");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return send_json(request, status_json());
}
esp_err_t download(httpd_req_t* request) {
    char query[160], requested[81];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "run_id", requested, sizeof(requested)) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "run_id query required");
    size_t length;
    {
        Guard guard;
        if (!header_valid || std::strcmp(requested, run_id) != 0 || stopping ||
            (state != State::stopped && state != State::error))
            return conflict(request, "stop and flush the selected recording first");
        length = kHeaderBytes + count * kRecordBytes;
    }
    // ESP HTTP server dispatches handlers serially, so prepare/ack cannot replace
    // this immutable log during download. Retry download from byte zero on loss.
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char buffer[1400];
    for (size_t offset = 0; offset < length;) {
        const size_t bytes = length - offset < sizeof(buffer) ? length - offset : sizeof(buffer);
        if (esp_partition_read(partition, offset, buffer, bytes) != ESP_OK) return ESP_FAIL;
        const esp_err_t result = httpd_resp_send_chunk(request, buffer, bytes);
        if (result != ESP_OK) return result;
        offset += bytes;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}
void discovery(void*) {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(kDiscoveryPort); address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (socket_fd < 0 || bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ESP_LOGE(kTag, "Discovery bind failed"); if (socket_fd >= 0) close(socket_fd); vTaskDelete(nullptr); return;
    }
    char buffer[512];
    while (true) {
        sockaddr_in peer{}; socklen_t peer_length = sizeof(peer);
        const int bytes = recvfrom(socket_fd, buffer, sizeof(buffer) - 1, 0,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (bytes <= 0 || bytes >= static_cast<int>(sizeof(buffer) - 1)) continue;
        buffer[bytes] = 0;
        cJSON* request = cJSON_ParseWithOpts(buffer, nullptr, true);
        const cJSON* version = cJSON_GetObjectItemCaseSensitive(request, "v");
        const cJSON* type = cJSON_GetObjectItemCaseSensitive(request, "type");
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "request_id");
        if (cJSON_IsNumber(version) && version->valuedouble == 1 && cJSON_IsString(type) &&
            std::strcmp(type->valuestring, "caster_recorder_discover") == 0 &&
            cJSON_IsString(id) && std::strlen(id->valuestring) <= 80) {
            cJSON* response = identity();
            cJSON_AddStringToObject(response, "type", "caster_recorder");
            cJSON_AddStringToObject(response, "request_id", id->valuestring);
            cJSON_AddNumberToObject(response, "http_port", kHttpPort);
            char* text = finish_json(response);
            if (text) { sendto(socket_fd, text, std::strlen(text), 0,
                              reinterpret_cast<sockaddr*>(&peer), peer_length); free(text); }
        }
        cJSON_Delete(request);
    }
}
}  // namespace

void recorder_init(const char* boot, int reset, Diagnostics* diagnostics) {
    std::snprintf(current_boot, sizeof(current_boot), "%s", boot);
    reset_reason = reset;
    counters = diagnostics;
    lock = xSemaphoreCreateMutex();
    write_queue = xQueueCreate(kQueueRecords, kRecordBytes);
    ESP_ERROR_CHECK(lock && write_queue ? ESP_OK : ESP_ERR_NO_MEM);
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "caster_log");
    journal_ready = nvs_open("caster_record", NVS_READWRITE, &prepare_journal) == ESP_OK;
    if (!partition || partition->size < kHeaderBytes + kFooterBytes + kRecordBytes) {
        partition = nullptr; state = State::error; std::strcpy(reason, "missing_log_partition");
        ESP_LOGE(kTag, "Flash the new custom partition table; local recording unavailable");
    } else if (!journal_ready) {
        state = State::error; std::strcpy(reason, "journal_unavailable");
    } else {
        capacity = (partition->size - kHeaderBytes - kFooterBytes) / kRecordBytes;
        recover();
    }
    ESP_ERROR_CHECK(xTaskCreate(writer, "log_writer", 6144, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.server_port = kHttpPort;
    http.stack_size = 8192;
    http.max_uri_handlers = 8;
    http.recv_wait_timeout = 10;
    http.send_wait_timeout = 10;
    http.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    const char* paths[] = {"/api/status", "/api/sync", "/api/prepare", "/api/start",
                           "/api/stop", "/api/ack", "/api/log"};
    for (size_t i = 0; i < 7; ++i) {
        httpd_uri_t uri{};
        uri.uri = paths[i];
        uri.method = i >= 2 && i <= 5 ? HTTP_POST : HTTP_GET;
        uri.handler = i == 0 ? get_status : i == 1 ? get_sync : i == 6 ? download : command;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }
    ESP_ERROR_CHECK(xTaskCreate(discovery, "log_discovery", 4096, nullptr, 2, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(kTag, "HTTP=%d discovery=%d state=%s capacity=%lu samples", kHttpPort, kDiscoveryPort,
             state_name(), static_cast<unsigned long>(capacity));
}
bool recorder_new_segment() { return new_segment.exchange(false); }
bool recorder_suppress_live() { return suppress_live.load(); }
void recorder_capture(const Sample& sample) {
    if (!tracking.load()) return;
    Guard guard;
    if (!tracking.load()) return;
    // Start can race an in-flight read. Ignore it until acquisition itself has
    // reset its segment, rather than claiming the old CENTER integration is new.
    const bool segment_ready = !new_segment.load();
    const int64_t timestamp = sample.reading.timestamp_us;
    const bool channel_times_in_window = timestamp >= start_us && (!stopping || timestamp <= stop_us)
#if CASTER_NODE_ID >= 4
        && (CASTER_NODE_ID == 5 || sample.reading.wheel.timestamp_us >= start_us)
        && (CASTER_NODE_ID == 5 || !stopping || sample.reading.wheel.timestamp_us <= stop_us)
#endif
        ;
    if (segment_ready && channel_times_in_window) {
        uint8_t record[kRecordBytes];
        encode_record(sample, record);
        if (accepted >= capacity) request_stop("capacity", timestamp, true);
        else if (xQueueSend(write_queue, record, 0) != pdTRUE) {
            counters->queue_drops.fetch_add(1);
            request_stop("buffer_overflow", timestamp, true);
            std::strcpy(reason, "buffer_overflow");
        } else {
            ++accepted;
            if (accepted == capacity) request_stop("capacity", timestamp, true);
        }
    }
    if (stopping) stop_seen = true;
}
}  // namespace caster

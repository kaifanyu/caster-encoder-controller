#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = -2;
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) throw std::runtime_error("ESP_ERROR_CHECK"); } while (0)
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
inline int64_t mock_time = 1000000;
inline int64_t esp_timer_get_time() { return mock_time; }
constexpr int pdTRUE = 1, pdPASS = 1, portMAX_DELAY = -1;
#define pdMS_TO_TICKS(x) (x)
using SemaphoreHandle_t = void*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<void*>(1); }
inline void xSemaphoreTake(SemaphoreHandle_t, int) {}
inline void xSemaphoreGive(SemaphoreHandle_t) {}
struct MockQueue { size_t capacity, item_size; std::deque<std::vector<uint8_t>> rows; };
using QueueHandle_t = MockQueue*;
inline QueueHandle_t xQueueCreate(size_t count, size_t size) { return new MockQueue{count, size, {}}; }
inline int xQueueSend(QueueHandle_t queue, const void* item, int) {
    if (queue->rows.size() == queue->capacity) return 0;
    const auto* first = static_cast<const uint8_t*>(item);
    queue->rows.emplace_back(first, first + queue->item_size); return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue, void* item, int) {
    if (queue->rows.empty()) return 0;
    std::memcpy(item, queue->rows.front().data(), queue->item_size); queue->rows.pop_front(); return pdTRUE;
}
inline size_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->rows.size(); }
inline void xQueueReset(QueueHandle_t queue) { queue->rows.clear(); }
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, void*) { return pdPASS; }
inline void (*mock_on_delay)() = nullptr;
inline void vTaskDelay(int ticks) { mock_time += ticks * 1000; if (mock_on_delay) mock_on_delay(); }
inline void vTaskDelete(void*) {}

struct esp_partition_t { size_t size; };
constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_ANY = 0xff;
inline esp_partition_t mock_partition{4096 * 5};
inline std::vector<uint8_t> mock_flash(mock_partition.size, 0xff);
inline int mock_write_budget = -1;
inline int mock_erase_calls = 0;
inline const esp_partition_t* esp_partition_find_first(int, int, const char*) { return &mock_partition; }
inline esp_err_t esp_partition_read(const esp_partition_t*, size_t offset, void* out, size_t size) {
    if (offset + size > mock_flash.size()) return ESP_FAIL;
    std::memcpy(out, mock_flash.data() + offset, size); return ESP_OK;
}
inline esp_err_t esp_partition_write(const esp_partition_t*, size_t offset, const void* input, size_t size) {
    if (offset + size > mock_flash.size()) return ESP_FAIL;
    const auto* data = static_cast<const uint8_t*>(input);
    for (size_t i = 0; i < size; ++i) {
        if (mock_write_budget == 0) return ESP_FAIL;
        if ((mock_flash[offset + i] & data[i]) != data[i]) return ESP_FAIL;
        mock_flash[offset + i] &= data[i];
        if (mock_write_budget > 0) --mock_write_budget;
    }
    return ESP_OK;
}
inline esp_err_t esp_partition_erase_range(const esp_partition_t*, size_t offset, size_t size) {
    if (offset + size > mock_flash.size() || offset % 4096 || size % 4096) return ESP_FAIL;
    std::fill(mock_flash.begin() + offset, mock_flash.begin() + offset + size, 0xff);
    ++mock_erase_calls; return ESP_OK;
}

struct httpd_req_t {
    const char* uri = "";
    size_t content_len = 0;
    std::string body, response, status = "200 OK";
    size_t consumed = 0;
};
using httpd_handle_t = void*;
constexpr int HTTP_GET = 0, HTTP_POST = 1;
constexpr int HTTPD_500_INTERNAL_SERVER_ERROR = 500, HTTPD_400_BAD_REQUEST = 400;
constexpr int HTTPD_RESP_USE_STRLEN = -1;
struct httpd_config_t {
    int server_port = 80, stack_size = 4096, max_uri_handlers = 8;
    int recv_wait_timeout = 5, send_wait_timeout = 5;
    bool lru_purge_enable = false;
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
struct httpd_uri_t { const char* uri; int method; esp_err_t (*handler)(httpd_req_t*); };
inline int httpd_start(httpd_handle_t* handle, const httpd_config_t*) { *handle = reinterpret_cast<void*>(1); return ESP_OK; }
inline int httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*) { return ESP_OK; }
inline int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
inline int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
inline int httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = status; return ESP_OK; }
inline int httpd_resp_send(httpd_req_t* req, const char* text, int length) {
    req->response.assign(text, length < 0 ? std::strlen(text) : static_cast<size_t>(length)); return ESP_OK;
}
inline int httpd_resp_send_err(httpd_req_t* req, int error, const char* text) {
    req->status = std::to_string(error); req->response = text; return ESP_OK;
}
inline int httpd_req_recv(httpd_req_t* req, char* buffer, size_t size) {
    const size_t n = std::min(size, req->body.size() - req->consumed);
    std::memcpy(buffer, req->body.data() + req->consumed, n); req->consumed += n; return n;
}
inline int httpd_req_get_url_query_str(httpd_req_t* req, char* output, size_t size) {
    const char* query = std::strchr(req->uri, '?');
    if (!query || std::strlen(++query) >= size) return ESP_FAIL;
    std::strcpy(output, query); return ESP_OK;
}
inline int httpd_query_key_value(const char* query, const char* key, char* output, size_t size) {
    const std::string prefix = std::string(key) + "=";
    if (std::strncmp(query, prefix.c_str(), prefix.size()) != 0) return ESP_FAIL;
    query += prefix.size();
    if (std::strlen(query) >= size) return ESP_FAIL;
    std::strcpy(output, query); return ESP_OK;
}
inline int httpd_resp_send_chunk(httpd_req_t* req, const char* data, size_t size) {
    if (size) req->response.append(data, size);
    return ESP_OK;
}

using socklen_t = unsigned;
struct sockaddr { int unused; };
struct sockaddr_in { int sin_family; uint16_t sin_port; struct { uint32_t s_addr; } sin_addr; };
constexpr int AF_INET = 2, SOCK_DGRAM = 2, IPPROTO_IP = 0, INADDR_ANY = 0;
inline uint16_t htons(uint16_t x) { return x; }
inline uint32_t htonl(uint32_t x) { return x; }
inline int socket(int, int, int) { return -1; }
inline int bind(int, const sockaddr*, size_t) { return 0; }
inline int recvfrom(int, void*, size_t, int, sockaddr*, socklen_t*) { return -1; }
inline int sendto(int, const void*, size_t n, int, const sockaddr*, socklen_t) { return static_cast<int>(n); }

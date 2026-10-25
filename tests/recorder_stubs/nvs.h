#pragma once
#include "recorder_sdk.h"
using nvs_handle_t = int;
constexpr int NVS_READWRITE = 1, ESP_ERR_NVS_NOT_FOUND = 1001;
inline std::string mock_prepare_intent;
inline std::string mock_pending_intent;
inline int mock_commit_budget = -1;
inline bool mock_nvs_read_error = false;
inline bool mock_nvs_set_error = false;
inline bool mock_nvs_erase_error = false;
inline esp_err_t nvs_open(const char*, int, nvs_handle_t* handle) {
    mock_pending_intent = mock_prepare_intent; *handle = 1; return ESP_OK;
}
inline esp_err_t nvs_set_str(nvs_handle_t, const char*, const char* text) {
    if (mock_nvs_set_error) return ESP_FAIL;
    mock_pending_intent = text; return ESP_OK;
}
inline esp_err_t nvs_get_str(nvs_handle_t, const char*, char* out, size_t* size) {
    if (mock_nvs_read_error) return ESP_FAIL;
    if (mock_prepare_intent.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < mock_prepare_intent.size() + 1) return ESP_FAIL;
    std::strcpy(out, mock_prepare_intent.c_str()); *size = mock_prepare_intent.size() + 1; return ESP_OK;
}
inline esp_err_t nvs_erase_key(nvs_handle_t, const char*) {
    if (mock_nvs_erase_error) return ESP_FAIL;
    mock_pending_intent.clear(); return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) {
    if (mock_commit_budget == 0) return ESP_FAIL;
    if (mock_commit_budget > 0) --mock_commit_budget;
    mock_prepare_intent = mock_pending_intent; return ESP_OK;
}

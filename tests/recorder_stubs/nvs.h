#pragma once
#include "recorder_sdk.h"
using nvs_handle_t = int;
constexpr int NVS_READWRITE = 1, ESP_ERR_NVS_NOT_FOUND = 1001;
inline std::string mock_prepare_intent;
inline esp_err_t nvs_open(const char*, int, nvs_handle_t* handle) { *handle = 1; return ESP_OK; }
inline esp_err_t nvs_set_str(nvs_handle_t, const char*, const char* text) { mock_prepare_intent = text; return ESP_OK; }
inline esp_err_t nvs_get_str(nvs_handle_t, const char*, char* out, size_t* size) {
    if (mock_prepare_intent.empty()) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < mock_prepare_intent.size() + 1) return ESP_FAIL;
    std::strcpy(out, mock_prepare_intent.c_str()); *size = mock_prepare_intent.size() + 1; return ESP_OK;
}
inline esp_err_t nvs_erase_key(nvs_handle_t, const char*) { mock_prepare_intent.clear(); return ESP_OK; }
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

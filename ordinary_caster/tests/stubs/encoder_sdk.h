#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#define CONFIG_IDF_TARGET_ESP32C6 1
#define CONFIG_PCNT_ISR_IRAM_SAFE 1
#define CONFIG_PCNT_CTRL_FUNC_IN_IRAM 1
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,4,2)
#define IRAM_ATTR
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_ARG = -1;
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) throw std::runtime_error("SDK failure"); } while (0)
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGW(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
inline int64_t mock_time = 100000;
inline int64_t esp_timer_get_time() { return mock_time; }
inline void esp_rom_delay_us(uint32_t us) { mock_time += us; }
using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
#define portENTER_CRITICAL(x) do { (void)(x); } while (0)
#define portEXIT_CRITICAL(x) do { (void)(x); } while (0)
#define pdMS_TO_TICKS(x) (x)
using TickType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;

using gpio_num_t = int;
constexpr int GPIO_NUM_0 = 0, GPIO_NUM_1 = 1, GPIO_NUM_20 = 20, GPIO_NUM_21 = 21;
constexpr int GPIO_MODE_OUTPUT = 1, GPIO_PULLUP_ENABLE = 1, GPIO_PULLDOWN_DISABLE = 0;
constexpr int GPIO_INTR_DISABLE = 0, GPIO_PULLDOWN_ONLY = 2;
struct gpio_config_t { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; };
inline int mock_spi_byte = 0;
inline bool mock_cs_high = true;
inline int gpio_set_level(gpio_num_t pin, int level) {
    assert(pin == 21);
    mock_cs_high = level;
    if (!level) mock_spi_byte = 0;
    return ESP_OK;
}
inline int gpio_config(const gpio_config_t* c) {
    assert(c->pin_bit_mask == (1ULL << 21)); return ESP_OK;
}
inline int gpio_pullup_dis(gpio_num_t pin) { assert(pin == 0 || pin == 1); return ESP_OK; }
inline int gpio_pulldown_dis(gpio_num_t pin) { assert(pin == 0 || pin == 1); return ESP_OK; }
inline int gpio_set_pull_mode(gpio_num_t pin, int mode) { assert(pin == 20 && mode == 2); return ESP_OK; }
inline int gpio_dump_io_configuration(FILE*, uint64_t) { return ESP_OK; }

using pcnt_unit_handle_t = void*;
using pcnt_channel_handle_t = void*;
struct pcnt_unit_config_t { int high_limit, low_limit, intr_priority; struct { bool accum_count; } flags; };
struct pcnt_glitch_filter_config_t { int max_glitch_ns; };
struct pcnt_chan_config_t { int edge_gpio_num, level_gpio_num; };
constexpr int PCNT_CHANNEL_EDGE_ACTION_INCREASE = 1, PCNT_CHANNEL_EDGE_ACTION_DECREASE = -1;
constexpr int PCNT_CHANNEL_LEVEL_ACTION_INVERSE = 1, PCNT_CHANNEL_LEVEL_ACTION_KEEP = 0;
inline int mock_count = 0;
inline bool mock_counter_error = false;
inline int mock_pcnt_init_count = 0, mock_pcnt_reads = 0, mock_spi_init_count = 0, mock_spi_reads = 0;
inline uint32_t mock_pending = 0;
inline int pcnt_new_unit(const pcnt_unit_config_t*, pcnt_unit_handle_t* p) { ++mock_pcnt_init_count; *p = reinterpret_cast<void*>(1); return ESP_OK; }
inline int pcnt_unit_set_glitch_filter(pcnt_unit_handle_t, const pcnt_glitch_filter_config_t*) { return ESP_OK; }
inline int pcnt_new_channel(pcnt_unit_handle_t, const pcnt_chan_config_t* c, pcnt_channel_handle_t* p) {
    assert((c->edge_gpio_num == 0 && c->level_gpio_num == 1) || (c->edge_gpio_num == 1 && c->level_gpio_num == 0));
    *p = reinterpret_cast<void*>(1); return ESP_OK;
}
inline int pcnt_channel_set_edge_action(pcnt_channel_handle_t, int, int) { return ESP_OK; }
inline int pcnt_channel_set_level_action(pcnt_channel_handle_t, int, int) { return ESP_OK; }
inline int pcnt_unit_add_watch_point(pcnt_unit_handle_t, int) { return ESP_OK; }
inline int pcnt_unit_enable(pcnt_unit_handle_t) { return ESP_OK; }
inline int pcnt_unit_clear_count(pcnt_unit_handle_t) { mock_count = 0; return ESP_OK; }
inline int pcnt_unit_start(pcnt_unit_handle_t) { return ESP_OK; }
inline int pcnt_unit_get_count(pcnt_unit_handle_t, int* result) { ++mock_pcnt_reads; *result = mock_count; return mock_counter_error ? -1 : ESP_OK; }
#define PCNT_LL_GET_HW(x) (x)
inline uint32_t pcnt_ll_get_intr_status(int) { return mock_pending; }

using spi_device_handle_t = void*;
struct spi_bus_config_t {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int data4_io_num, data5_io_num, data6_io_num, data7_io_num, max_transfer_sz;
};
struct spi_device_interface_config_t { int clock_speed_hz, mode, spics_io_num, queue_size; };
struct spi_transaction_t { int flags, length; uint8_t tx_data[4], rx_data[4]; };
constexpr int SPI2_HOST = 2, SPI_DMA_DISABLED = 0;
constexpr int SPI_TRANS_USE_TXDATA = 1, SPI_TRANS_USE_RXDATA = 2;
inline uint16_t mock_word = 0xc000;
inline bool mock_spi_error = false, mock_bus_error = false;
inline int spi_bus_initialize(int, const spi_bus_config_t* c, int) {
    ++mock_spi_init_count;
    assert(c->mosi_io_num == 18 && c->miso_io_num == 20 && c->sclk_io_num == 19);
    return ESP_OK;
}
inline int spi_bus_add_device(int, const spi_device_interface_config_t* c, spi_device_handle_t* p) {
    assert(c->mode == 0 && c->clock_speed_hz == 500000 && c->spics_io_num == -1);
    *p = reinterpret_cast<void*>(1); return ESP_OK;
}
inline int spi_device_acquire_bus(spi_device_handle_t, TickType_t wait) {
    // ESP-IDF 5.4.2 rejects every finite acquire timeout before any SPI I/O.
    if (wait != portMAX_DELAY) return ESP_ERR_INVALID_ARG;
    return mock_bus_error ? -1 : ESP_OK;
}
inline void spi_device_release_bus(spi_device_handle_t) { assert(mock_cs_high); }
inline int spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t* t) {
    ++mock_spi_reads;
    assert(!mock_cs_high && t->length == 8 && t->tx_data[0] == 0);
    t->rx_data[0] = mock_spi_byte++ == 0 ? mock_word >> 8 : mock_word & 0xff;
    return mock_spi_error ? -1 : ESP_OK;
}

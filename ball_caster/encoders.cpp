#include "encoders.h"
#include "encoder_math.h"

#include <cmath>
#include <cstdio>
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/pcnt_ll.h"

#if !CONFIG_IDF_TARGET_ESP32C6 || ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 4, 2)
#error "PCNT snapshot implementation is audited for ESP32-C6 / ESP-IDF 5.4.2 only"
#endif

namespace caster {
namespace {
constexpr gpio_num_t kA = GPIO_NUM_0;
constexpr gpio_num_t kB = GPIO_NUM_1;
constexpr gpio_num_t kZ = GPIO_NUM_2;
constexpr gpio_num_t kCs = GPIO_NUM_21;
constexpr int kCounterLimit = 30000;
// Conservative upper bound of one count per 80 MHz APB cycle, independent
// of an operator's unverified RPM estimate. Stay strictly below 2^31 counts.
constexpr int64_t kMaximumSnapshotGapUs = 26000000;
constexpr const char* kTag = "encoders";
EncoderConfig config{};
pcnt_unit_handle_t counter = nullptr;
spi_device_handle_t spi = nullptr;
portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
math::CountExtender extended_count;
math::AbsoluteTracker absolute_tracker;
int64_t last_count_snapshot_us = 0;
bool count_continuity_lost = false;
uint64_t index_events = 0;
int64_t index_timestamp = 0;
int64_t index_count = 0;
bool index_valid = false;
uint32_t invalid_spi = 0;
int64_t last_cs_high_us = 0;
bool previous_sample_valid = false;
int64_t previous_sample_time = 0;
int64_t previous_sample_count = 0;
double filtered_velocity = missing_value;

// Must be called with snapshot_lock held. Public get_count() alone can read
// a hardware counter just reset at a limit before the driver's ISR updates
// its accumulator. Observe pending interrupts on both sides and reject that
// snapshot. No register is cleared or changed here. Checking the whole group
// also avoids depending on opaque driver handle layout/unit allocation.
//
// The single-core C6 PCNT ISR runs at priority 3 and Z at priority 1: Z cannot
// preempt the driver's clear-pending -> accumulate interval. The task cannot
// run inside an ISR. This is why this helper is pinned to this target/SDK.
bool IRAM_ATTR count_snapshot(int64_t& count, int64_t& timestamp) {
    timestamp = esp_timer_get_time();
    if (timestamp - last_count_snapshot_us > kMaximumSnapshotGapUs) {
        count_continuity_lost = true;
    }
    if (count_continuity_lost || pcnt_ll_get_intr_status(PCNT_LL_GET_HW(0))) return false;
    int driver_count = 0;
    if (pcnt_unit_get_count(counter, &driver_count) != ESP_OK) return false;
    if (pcnt_ll_get_intr_status(PCNT_LL_GET_HW(0))) return false;
    count = extended_count.update(static_cast<uint32_t>(driver_count));
    last_count_snapshot_us = timestamp;
    return true;
}

void IRAM_ATTR index_isr(void*) {
    portENTER_CRITICAL_ISR(&snapshot_lock);
    ++index_events;
    // Timestamp/count describe GPIO interrupt service, not hardware-latched
    // edge time. Latest event is retained; cumulative event count never resets.
    index_valid = count_snapshot(index_count, index_timestamp);
    portEXIT_CRITICAL_ISR(&snapshot_lock);
}

void init_quadrature() {
    const double edge_interval_ns = 60e9 / (4.0 * config.ppr * config.max_rpm);
    if (config.glitch_filter_ns > 0 && config.glitch_filter_ns >= edge_interval_ns) {
        ESP_LOGE(kTag, "Glitch filter exceeds x4 edge interval at configured PPR/RPM");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }
    if (config.glitch_filter_ns > 0 && (!config.ppr_verified || !config.max_rpm_verified)) {
        ESP_LOGW(kTag, "Filter enabled against unverified PPR/RPM: bench verification required");
    }
    pcnt_unit_config_t unit_config{};
    unit_config.high_limit = kCounterLimit;
    unit_config.low_limit = -kCounterLimit;
    unit_config.intr_priority = 3;
    unit_config.flags.accum_count = true;
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &counter));
    if (config.glitch_filter_ns > 0) {
        pcnt_glitch_filter_config_t filter{};
        filter.max_glitch_ns = config.glitch_filter_ns;
        ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(counter, &filter));
    }
    pcnt_chan_config_t channel_config{};
    channel_config.edge_gpio_num = kA;
    channel_config.level_gpio_num = kB;
    pcnt_channel_handle_t channel_a = nullptr;
    pcnt_channel_handle_t channel_b = nullptr;
    ESP_ERROR_CHECK(pcnt_new_channel(counter, &channel_config, &channel_a));
    channel_config.edge_gpio_num = kB;
    channel_config.level_gpio_num = kA;
    ESP_ERROR_CHECK(pcnt_new_channel(counter, &channel_config, &channel_b));
    // A leads B => positive: 00 -> 10 -> 11 -> 01 -> 00.
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(channel_a, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(channel_a, PCNT_CHANNEL_LEVEL_ACTION_INVERSE,
                                                 PCNT_CHANNEL_LEVEL_ACTION_KEEP));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(channel_b, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(channel_b, PCNT_CHANNEL_LEVEL_ACTION_INVERSE,
                                                 PCNT_CHANNEL_LEVEL_ACTION_KEEP));
    // The external buffer drives A/B. IDF may enable pulls when connecting
    // channel inputs; explicitly disable them after both channels exist.
    const gpio_num_t driven_inputs[] = {kA, kB};
    for (gpio_num_t pin : driven_inputs) {
        ESP_ERROR_CHECK(gpio_pullup_dis(pin));
        ESP_ERROR_CHECK(gpio_pulldown_dis(pin));
    }
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(counter, kCounterLimit));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(counter, -kCounterLimit));
    ESP_ERROR_CHECK(pcnt_unit_enable(counter));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(counter));  // Once at startup only.
    last_count_snapshot_us = esp_timer_get_time();
    ESP_ERROR_CHECK(pcnt_unit_start(counter));

    gpio_config_t index_gpio{};
    index_gpio.pin_bit_mask = 1ULL << kZ;
    index_gpio.mode = GPIO_MODE_INPUT;
    index_gpio.intr_type = GPIO_INTR_POSEDGE;
    ESP_ERROR_CHECK(gpio_config(&index_gpio));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(kZ, index_isr, nullptr));
}

void init_absolute() {
    // Manual CS is not configured by the SPI driver (spics_io_num = -1).
    // gpio_set_direction() alone does not select the GPIO IO_MUX function;
    // GPIO21 can remain routed to SDIO_DATA1 instead of driving encoder CS.
    // Preload HIGH, then configure the mux and output together to avoid a
    // spurious active-low select during initialization.
    ESP_ERROR_CHECK(gpio_set_level(kCs, 1));
    gpio_config_t cs_config{};
    cs_config.pin_bit_mask = 1ULL << kCs;
    cs_config.mode = GPIO_MODE_OUTPUT;
    cs_config.pull_up_en = GPIO_PULLUP_ENABLE;
    cs_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cs_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cs_config));
    spi_bus_config_t bus{};
    bus.mosi_io_num = 18;
    bus.miso_io_num = 20;
    bus.sclk_io_num = 19;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = 1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
    // Bias an otherwise floating/unplugged MISO toward the invalid 0x0000
    // response. Parity still cannot prove physical sensor presence.
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_NUM_20, GPIO_PULLDOWN_ONLY));
    spi_device_interface_config_t device{};
    device.clock_speed_hz = 500000;
    device.mode = 0;
    device.spics_io_num = -1;  // Manual CS stays LOW across two transactions.
    device.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &spi));
    last_cs_high_us = esp_timer_get_time();
    ESP_LOGI(kTag, "CENTER SPI: mode=0 clock=500000Hz; manual CS GPIO21 configured as GPIO");
    ESP_ERROR_CHECK(gpio_dump_io_configuration(stdout,
        (1ULL << kCs) | (1ULL << 19) | (1ULL << 20) | (1ULL << 18)));
}

bool read_absolute_word(uint16_t& word, int64_t& timestamp, unsigned startup_probe = 0) {
    const int64_t remaining_us = 40 - (esp_timer_get_time() - last_cs_high_us);
    if (remaining_us > 0) esp_rom_delay_us(static_cast<uint32_t>(remaining_us));
    ESP_ERROR_CHECK(spi_device_acquire_bus(spi, portMAX_DELAY));
    const esp_err_t cs_low_result = gpio_set_level(kCs, 0);
    timestamp = esp_timer_get_time();  // Local acquisition time, before first clock.
    esp_rom_delay_us(3);
    spi_transaction_t high{};
    high.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    high.length = 8;
    high.tx_data[0] = 0x00;
    const esp_err_t high_result = cs_low_result == ESP_OK
        ? spi_device_polling_transmit(spi, &high) : cs_low_result;
    esp_rom_delay_us(3);
    spi_transaction_t low{};
    low.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    low.length = 8;
    low.tx_data[0] = 0x00;
    const esp_err_t low_result = high_result == ESP_OK
        ? spi_device_polling_transmit(spi, &low) : high_result;
    esp_rom_delay_us(3);
    const esp_err_t cs_high_result = gpio_set_level(kCs, 1);
    last_cs_high_us = esp_timer_get_time();
    spi_device_release_bus(spi);
    word = static_cast<uint16_t>((high.rx_data[0] << 8) | low.rx_data[0]);
    const bool io_ok = cs_low_result == ESP_OK && high_result == ESP_OK &&
                       low_result == ESP_OK && cs_high_result == ESP_OK;
    // Boot-only probes use the production transaction, with logging after CS
    // release. No printing is added to the periodic acquisition path.
    if (startup_probe) {
        ESP_LOGI(kTag, "CENTER SPI probe=%u tx=00,00 rx=%02x,%02x cs_low=%s byte1=%s byte2=%s cs_high=%s parity=%s",
                 startup_probe, static_cast<unsigned>(high.rx_data[0]),
                 static_cast<unsigned>(low.rx_data[0]), esp_err_to_name(cs_low_result),
                 cs_low_result == ESP_OK ? esp_err_to_name(high_result) : "skipped",
                 high_result == ESP_OK ? esp_err_to_name(low_result) : "skipped",
                 esp_err_to_name(cs_high_result),
                 io_ok ? (math::amt25_parity_valid(word) ? "valid" : "invalid") : "not_checked");
    }
    return io_ok;
}
}  // namespace

void encoder_init(const EncoderConfig& settings) {
    config = settings;
    if (config.role < 1 || config.role > 3 || config.ppr <= 0 ||
        (config.sign != 1 && config.sign != -1) || config.glitch_filter_ns < 0 ||
        !std::isfinite(config.max_rpm) || config.max_rpm <= 0 ||
        !std::isfinite(config.alpha_zero_rad) ||
        !std::isfinite(config.velocity_filter_tau_s) || config.velocity_filter_tau_s < 0) {
        ESP_LOGE(kTag, "Invalid encoder configuration");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }
    if (config.role == 3) {
        init_absolute();
        // Diagnose raw reception before the sample timer starts. These reads
        // do not seed the angle tracker or enter telemetry/recording counters.
        for (unsigned probe = 1; probe <= 3; ++probe) {
            uint16_t word = 0;
            int64_t timestamp = 0;
            read_absolute_word(word, timestamp, probe);
        }
    } else init_quadrature();
}

void encoder_begin_recording_segment() {
    absolute_tracker = math::AbsoluteTracker{};
    previous_sample_valid = false;
    previous_sample_time = 0;
    filtered_velocity = missing_value;
}

Reading encoder_read() {
    Reading result;
    result.flags = READY;
    if (config.max_rpm_verified) result.flags |= RATE_BOUND_VERIFIED;
    if (config.phase_reference_valid) result.flags |= PHASE_REFERENCE_VALID;
    if (config.role != 3) {
        if (config.ppr_verified) result.flags |= PPR_VERIFIED;
        portENTER_CRITICAL(&snapshot_lock);
        const bool valid = count_snapshot(result.raw_count, result.timestamp_us);
        result.index_event_count = index_events;
        result.index_timestamp_us = index_timestamp;
        result.index_raw_count = index_count;
        if (index_valid) result.flags |= INDEX_VALID;
        if (count_continuity_lost) result.flags |= CONTINUITY_LOST;
        portEXIT_CRITICAL(&snapshot_lock);
        // No last-good count is repeated as a valid sample. A pending PCNT ISR
        // can invalidate one sample; the next snapshot preserves total counts.
        if (!valid) return result;
        result.flags |= SAMPLE_VALID | UNWRAP_CONTINUOUS;
        result.angle_rad = math::quadrature_angle(result.raw_count, config.zero_count,
                                                  config.ppr, config.sign);
        if (previous_sample_valid && result.timestamp_us > previous_sample_time) {
            const double dt_s = (result.timestamp_us - previous_sample_time) / 1e6;
            result.velocity_rad_s = math::quadrature_velocity(result.raw_count, previous_sample_count,
                result.timestamp_us - previous_sample_time, config.ppr, config.sign);
            filtered_velocity = math::low_pass(filtered_velocity, result.velocity_rad_s,
                                               dt_s, config.velocity_filter_tau_s);
            result.filtered_velocity_rad_s = filtered_velocity;
            result.flags |= VELOCITY_VALID;
        }
        previous_sample_count = result.raw_count;
    } else {
        const bool io_ok = read_absolute_word(result.raw_spi_word, result.timestamp_us);
        if (!io_ok || !math::amt25_parity_valid(result.raw_spi_word)) {
            result.flags |= io_ok ? PARITY_ERROR : SPI_IO_ERROR;
            result.invalid_spi_responses = ++invalid_spi;
            // Tracker retains the last valid timestamp. The next valid word
            // must prove that the entire intervening gap was unambiguous.
            return result;
        }
        result.flags |= SAMPLE_VALID;
        result.invalid_spi_responses = invalid_spi;
        result.position14 = result.raw_spi_word & 0x3fff;
        result.absolute_angle_rad = result.position14 * math::tau / math::absolute_bins;
        result.calibrated_alpha_rad = math::wrap_positive(config.sign *
            (result.absolute_angle_rad - config.alpha_zero_rad));
        const auto step = absolute_tracker.update(result.position14, result.timestamp_us,
            config.max_rpm, config.max_rpm_verified, config.sign, config.alpha_zero_rad);
        result.continuity_loss = absolute_tracker.continuity_loss();
        if (step.continuous) result.flags |= UNWRAP_CONTINUOUS;
        else result.flags |= CONTINUITY_LOST;
        result.angle_rad = step.angle_rad;
        if (step.velocity_valid) {
            const double dt_s = (result.timestamp_us - previous_sample_time) / 1e6;
            result.velocity_rad_s = step.velocity_rad_s;
            filtered_velocity = math::low_pass(filtered_velocity, step.velocity_rad_s,
                                               dt_s, config.velocity_filter_tau_s);
            result.filtered_velocity_rad_s = filtered_velocity;
            result.flags |= VELOCITY_VALID;
        } else filtered_velocity = missing_value;
    }
    previous_sample_time = result.timestamp_us;
    previous_sample_valid = true;
    return result;
}

}  // namespace caster

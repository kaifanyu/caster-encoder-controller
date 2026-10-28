#include "encoders.h"
#include "encoder_math.h"

#include <cmath>
#include <cstdio>
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
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
#if !CONFIG_PCNT_ISR_IRAM_SAFE || !CONFIG_PCNT_CTRL_FUNC_IN_IRAM
#error "Ordinary wheel counting during flash recording requires IRAM-safe PCNT"
#endif

namespace caster {
namespace {
constexpr gpio_num_t kA = GPIO_NUM_0;
constexpr gpio_num_t kB = GPIO_NUM_1;
constexpr gpio_num_t kCs = GPIO_NUM_21;
constexpr int kCounterLimit = 30000;
// Conservative upper bound of one count per 80 MHz APB cycle, independent
// of an operator's unverified RPM estimate. Stay strictly below 2^31 counts.
constexpr int64_t kMaximumSnapshotGapUs = 26000000;
constexpr const char* kTag = "ordinary_encoders";
EncoderConfig config{};
pcnt_unit_handle_t counter = nullptr;
spi_device_handle_t spi = nullptr;
portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
math::CountExtender extended_count;
ordinary::SwivelTracker swivel_tracker;
ordinary::WheelVelocityEstimator wheel_velocity;
int64_t last_count_snapshot_us = 0;
bool count_continuity_lost = false;
uint32_t invalid_spi = 0;
int64_t last_cs_high_us = 0;
int64_t previous_sample_time = 0;
int64_t previous_wheel_velocity_time = 0;
double filtered_wheel_velocity = missing_value;
double filtered_velocity = missing_value;

// Must be called with snapshot_lock held. Public get_count() alone can read
// a hardware counter just reset at a limit before the driver's ISR updates
// its accumulator. Observe pending interrupts on both sides and reject that
// snapshot. No register is cleared or changed here. Checking the whole group
// also avoids depending on opaque driver handle layout/unit allocation.
//
// The single-core C6 acquisition task cannot run inside the PCNT ISR.
// This snapshot contract is pinned to this target/SDK. No Z input is used.
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

void init_quadrature() {
    if (config.glitch_filter_ns > 0) {
        ESP_LOGW(kTag, "Hall filter enabled: verify its width against actual wheel edge spacing");
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
    // DRV5013 open-drain outputs require external 4.7 kohm pull-ups to
    // 3.3 V. Disable internal pulls; they do not replace the specified circuit.
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
    ESP_LOGI(kTag, "AMT222A-V SPI mode=0 500000Hz CS=21 SCLK=19 MISO=20 MOSI=18");
    ESP_ERROR_CHECK(gpio_dump_io_configuration(stdout, 1ULL << kCs));
}

bool read_absolute_word(uint16_t& word, int64_t& timestamp) {
    const int64_t remaining_us = 40 - (esp_timer_get_time() - last_cs_high_us);
    if (remaining_us > 0) esp_rom_delay_us(static_cast<uint32_t>(remaining_us));
    timestamp = esp_timer_get_time();
    word = 0;
    // ESP-IDF 5.4.2 supports only portMAX_DELAY for bus acquisition. This
    // acquisition task is the sole owner; hold the bus across both SPI bytes.
    if (spi_device_acquire_bus(spi, portMAX_DELAY) != ESP_OK) return false;
    gpio_set_level(kCs, 0);
    timestamp = esp_timer_get_time();  // Local acquisition time, before first clock.
    esp_rom_delay_us(3);
    spi_transaction_t high{};
    high.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    high.length = 8;
    high.tx_data[0] = 0x00;
    const esp_err_t high_result = spi_device_polling_transmit(spi, &high);
    esp_rom_delay_us(3);
    spi_transaction_t low{};
    low.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    low.length = 8;
    low.tx_data[0] = 0x00;
    const esp_err_t low_result = high_result == ESP_OK
        ? spi_device_polling_transmit(spi, &low) : high_result;
    esp_rom_delay_us(3);
    gpio_set_level(kCs, 1);
    last_cs_high_us = esp_timer_get_time();
    spi_device_release_bus(spi);
    word = static_cast<uint16_t>((high.rx_data[0] << 8) | low.rx_data[0]);
    return high_result == ESP_OK && low_result == ESP_OK;
}
}  // namespace

void encoder_init(const EncoderConfig& settings) {
    config = settings;
    if ((config.sign != 1 && config.sign != -1) ||
        (config.wheel_sign != 1 && config.wheel_sign != -1) || config.wheel_cpr <= 0 ||
        config.wheel_velocity_window_us < ordinary::minimum_velocity_window_us ||
        config.wheel_velocity_window_us > ordinary::maximum_velocity_window_us ||
        config.glitch_filter_ns < 0 || !std::isfinite(config.max_rpm) || config.max_rpm <= 0 ||
        !std::isfinite(config.alpha_zero_rad) || !std::isfinite(config.velocity_filter_tau_s) ||
        config.velocity_filter_tau_s < 0) {
        ESP_LOGE(kTag, "Invalid ordinary-caster encoder configuration");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }
    if (config.role != 5) init_quadrature();
    if (config.role != 6) init_absolute();
}

void encoder_begin_recording_segment() {
    swivel_tracker = ordinary::SwivelTracker{};
    wheel_velocity.reset();
    previous_sample_time = 0;
    previous_wheel_velocity_time = 0;
    filtered_velocity = filtered_wheel_velocity = missing_value;
    // Hall count and its continuity state remain cumulative for this boot.
}

Reading encoder_read() {
    Reading result;
    // Snapshot the independent wheel channel even when the SPI encoder is
    // disconnected. Each channel keeps its own actual acquisition timestamp.
    auto& wheel = result.wheel;
    if (config.role != 5) {
    wheel.flags = READY;
    if (config.wheel_cpr_verified) wheel.flags |= PPR_VERIFIED;
    portENTER_CRITICAL(&snapshot_lock);
    const bool wheel_valid = count_snapshot(wheel.raw_count, wheel.timestamp_us);
    if (count_continuity_lost) wheel.flags |= CONTINUITY_LOST;
    portEXIT_CRITICAL(&snapshot_lock);
    if (wheel_valid) {
        wheel.flags |= SAMPLE_VALID | UNWRAP_CONTINUOUS;
        wheel.angle_rad = ordinary::wheel_angle(wheel.raw_count, config.wheel_zero_count,
                                               config.wheel_cpr, config.wheel_sign);
        const auto velocity = wheel_velocity.update(wheel.raw_count, wheel.timestamp_us,
            config.wheel_cpr, config.wheel_sign, config.wheel_velocity_window_us);
        if (velocity.valid) {
            wheel.velocity_rad_s = velocity.radians_per_second;
            const double dt_s = previous_wheel_velocity_time > 0
                ? (wheel.timestamp_us - previous_wheel_velocity_time) / 1e6
                : velocity.interval_us / 1e6;
            filtered_wheel_velocity = math::low_pass(filtered_wheel_velocity,
                velocity.radians_per_second, dt_s, config.velocity_filter_tau_s);
            wheel.filtered_velocity_rad_s = filtered_wheel_velocity;
            wheel.flags |= VELOCITY_VALID;
            previous_wheel_velocity_time = wheel.timestamp_us;
        } else {
            filtered_wheel_velocity = missing_value;
            previous_wheel_velocity_time = 0;
        }
    } else {
        wheel_velocity.reset();
        filtered_wheel_velocity = missing_value;
        previous_wheel_velocity_time = 0;
    }
    }
    if (config.role == 6) {
        result.timestamp_us = wheel.timestamp_us;
        return result; // Wheel-only board never touches SPI or the swivel pins.
    }

    result.flags = READY;
    if (config.max_rpm_verified) result.flags |= RATE_BOUND_VERIFIED;
    if (config.phase_reference_valid) result.flags |= PHASE_REFERENCE_VALID;

    const bool io_ok = read_absolute_word(result.raw_spi_word, result.timestamp_us);
    const bool parity_ok = math::amt25_parity_valid(result.raw_spi_word);
    if (!io_ok || !parity_ok || (result.raw_spi_word & 3u) != 0) {
        result.flags |= !io_ok ? SPI_IO_ERROR : !parity_ok ? PARITY_ERROR : FORMAT_ERROR;
        result.invalid_spi_responses = ++invalid_spi;
        // Keep the last valid timestamp in the tracker: a later good read must
        // prove that the whole intervening interval was short enough to unwrap.
        return result;
    }
    result.flags |= SAMPLE_VALID;
    result.invalid_spi_responses = invalid_spi;
    result.position14 = result.raw_spi_word & 0x3fff;
    result.position12 = ordinary::position12(result.raw_spi_word);
    result.absolute_angle_rad = result.position12 * math::tau / ordinary::swivel_bins;
    result.calibrated_alpha_rad = ordinary::wrap_signed(config.sign *
        (result.absolute_angle_rad - config.alpha_zero_rad));
    const auto step = swivel_tracker.update(result.position12, result.timestamp_us,
        config.max_rpm, config.max_rpm_verified, config.sign, config.alpha_zero_rad);
    result.continuity_loss = swivel_tracker.continuity_loss();
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
    previous_sample_time = result.timestamp_us;
    return result;
}

}  // namespace caster

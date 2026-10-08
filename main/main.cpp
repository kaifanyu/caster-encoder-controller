#include "config.h"
#include "telemetry.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstdlib>
#include <cinttypes>

namespace {
constexpr const char* TAG = "caster";
QueueHandle_t samples_queue;
TaskHandle_t acquisition_task;
caster::Diagnostics diagnostics;
char boot_id[17];
int reset_reason;

void timer_callback(void*) { xTaskNotifyGive(acquisition_task); }

void acquire(void*) {
    uint64_t sequence = 0;
    int64_t previous_us = 0;
    while (true) {
        const uint32_t pending = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        caster::Sample sample{};
        // Missed timer notifications correspond to skipped acquisition slots.
        sequence += pending - 1;
        sample.sequence = sequence++;
        sample.reading = caster::encoder_read();
        const int64_t interval = sample.reading.timestamp_us - previous_us;
        if (pending > 1 || (previous_us != 0 && interval > 1500000 / SAMPLE_HZ)) {
            diagnostics.acquisition_overruns.fetch_add(pending > 1 ? pending - 1 : 1);
            sample.reading.flags |= caster::ACQUISITION_OVERRUN;
        }
        previous_us = sample.reading.timestamp_us;
        diagnostics.invalid_spi_responses.store(sample.reading.invalid_spi_responses);
        diagnostics.index_queue_drops.store(sample.reading.index_queue_drops);
        if (xQueueSend(samples_queue, &sample, 0) != pdTRUE)
            diagnostics.queue_drops.fetch_add(1);
    }
}

void transmit(void*) {
    TickType_t wake = xTaskGetTickCount();
    int64_t last_metadata = -2000000;
    int64_t last_debug = 0;
    int64_t last_reconnect = 0;
    const auto config = caster::configuration();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / TRANSMIT_HZ));
        caster::Sample batch[2];
        int count = 0;
        while (count < 2 && xQueueReceive(samples_queue, &batch[count], 0) == pdTRUE) ++count;
        if (count) {
            char* data = caster::serialize_samples(boot_id, batch, count, diagnostics);
            if (!caster::transport_send(data)) diagnostics.radio_failures.fetch_add(1);
            free(data);
        }
        const int64_t now = esp_timer_get_time();
        if (now - last_reconnect >= 2000000) {
            caster::transport_reconnect();
            last_reconnect = now;
        }
        if (now - last_metadata >= 2000000) {
            char* data = caster::serialize_metadata(boot_id, config, reset_reason);
            if (!caster::transport_send(data)) diagnostics.radio_failures.fetch_add(1);
            free(data);
            last_metadata = now;
        }
        if (now - last_debug >= 1000000) {
            ESP_LOGI(TAG, "wifi=%s overruns=%" PRIu32 " queue_drops=%" PRIu32
                     " spi_invalid=%" PRIu32 " radio_failures=%" PRIu32,
                     caster::transport_connected() ? "up" : "down",
                     diagnostics.acquisition_overruns.load(), diagnostics.queue_drops.load(),
                     diagnostics.invalid_spi_responses.load(), diagnostics.radio_failures.load());
            if (count) {
                const auto& r = batch[count - 1].reading;
                ESP_LOGI(TAG, "seq=%" PRIu64 " count=%" PRId64 " spi=0x%04x pos=%d angle=%.6f velocity=%.6f flags=0x%04" PRIx32,
                         batch[count - 1].sequence, r.raw_count, r.raw_spi_word, r.position14,
                         r.angle_rad, r.velocity_rad_s, r.flags);
            }
            last_debug = now;
        }
    }
}
}  // namespace

extern "C" void app_main() {
    // GPIO3 is the internal RF control, NOT D3/GPIO21 (encoder CS).
    gpio_config_t rf{};
    rf.pin_bit_mask = 1ULL << 3;
    rf.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&rf));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_3, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    rf.pin_bit_mask = 1ULL << 14;
    ESP_ERROR_CHECK(gpio_config(&rf));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_14, 0));
    reset_reason = static_cast<int>(esp_reset_reason());
    const auto config = caster::configuration();
    ESP_LOGI(TAG, "firmware=%s IDF=%s role=%s node=%d reset_reason=%d",
             caster::kFirmwareVersion, esp_get_idf_version(), caster::kRole, CASTER_NODE_ID, reset_reason);
    ESP_LOGI(TAG, "GPIO: %s; onboard RF GPIO3=0 GPIO14=0",
             CASTER_NODE_ID == 3 ? "CS=21 SCLK=19 MISO=20 MOSI=18" : "A=0 B=1 Z=2");
    if (CASTER_NODE_ID != 3)
        ESP_LOGI(TAG, "PPR=%d x4=%d verified=%s sign=%d", config.ppr, config.ppr * 4,
                 config.ppr_verified ? "yes" : "NO", config.sign);
    ESP_LOGI(TAG, "Hold mechanism stationary at power-on; waiting 250 ms. USB not required.");
    vTaskDelay(pdMS_TO_TICKS(250));
    caster::encoder_init(config);
    samples_queue = xQueueCreate(64, sizeof(caster::Sample));
    ESP_ERROR_CHECK(samples_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(acquire, "acquisition", 6144, nullptr, 12, &acquisition_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    esp_timer_handle_t timer;
    esp_timer_create_args_t args{};
    args.callback = timer_callback;
    args.name = "sample_clock";
    // Task-dispatched callbacks are short; acquisition runs in its own task.
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000000 / SAMPLE_HZ));
    caster::transport_init();
    // Generate the session identifier with the RF entropy source active.
    snprintf(boot_id, sizeof(boot_id), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    ESP_LOGI(TAG, "boot_id=%s UDP receiver=%s:%d sample=%dHz transmit=%dHz clock_sync=INVALID",
             boot_id, RECEIVER_IPV4, UDP_PORT, SAMPLE_HZ, TRANSMIT_HZ);
    ESP_ERROR_CHECK(xTaskCreate(transmit, "telemetry", 8192, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

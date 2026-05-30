#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_psram.h"

#include "app_version.h"

static const char *TAG = "clawlexa";

/* Boot banner — pytest-embedded asserts on the literal "clawlexa booted"
 * substring, so don't change that phrase without updating tests/pytest. */
static void log_boot_banner(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "clawlexa booted");
    ESP_LOGI(TAG, "  version : %s", app_version());
    ESP_LOGI(TAG, "  chip    : ESP32-S3 rev %d, %d cores",
             chip.revision, chip.cores);
    ESP_LOGI(TAG, "  psram   : %u bytes",
             (unsigned)esp_psram_get_size());
}

void app_main(void) {
    log_boot_banner();

    /* Phase 1a: heartbeat only. Display / audio bring-up lands in 1b/1c. */
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t tick = 0;
    while (1) {
        ESP_LOGI(TAG, "heartbeat %lu", (unsigned long)tick++);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5000));
    }
}

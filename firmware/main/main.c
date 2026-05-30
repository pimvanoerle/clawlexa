#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_psram.h"

#include "app_version.h"
#include "display.h"
#include "touch.h"

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

#if CONFIG_CLAWLEXA_HEADLESS
    /* Emulation / host-CI build: no LCD or touch hardware present. Skip their
     * bring-up so the boot path (logging, heartbeat) can be smoke-tested in
     * QEMU without the peripheral init aborting. */
    ESP_LOGI(TAG, "headless build: skipping display/touch bring-up");
#else
    /* Phase 1b: bring up the display, then touch on the shared I2C bus. */
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(display_init(&i2c_bus));
    ESP_ERROR_CHECK(touch_init(i2c_bus));
#endif

    /* Phase 1a: heartbeat only. Audio bring-up lands in 1c. */
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t tick = 0;
    while (1) {
        ESP_LOGI(TAG, "heartbeat %lu", (unsigned long)tick++);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5000));
    }
}

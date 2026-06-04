#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_system.h"

#include "app_version.h"
#include "display.h"
#include "touch.h"
#include "audio.h"
#include "mic.h"
#include "wifi.h"
#include "ws.h"

static const char *TAG = "clawlexa";

/* Boot sound embedded from main/assets/boot.wav (see EMBED_FILES in CMakeLists). */
extern const uint8_t boot_wav_start[] asm("_binary_boot_wav_start");
extern const uint8_t boot_wav_end[]   asm("_binary_boot_wav_end");

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
    /* esp_reset_reason() == ESP_RST_BROWNOUT (9) confirms a brownout reset; on a
     * clean cold start it's ESP_RST_POWERON (1). Logged every boot for diagnosis. */
    ESP_LOGI(TAG, "reset reason: %d", (int)esp_reset_reason());

#if CONFIG_CLAWLEXA_HEADLESS
    /* Emulation / host-CI build: no LCD or touch hardware present. Skip their
     * bring-up so the boot path (logging, heartbeat) can be smoke-tested in
     * QEMU without the peripheral init aborting. */
    ESP_LOGI(TAG, "headless build: skipping display/touch bring-up");
#else
    /* Brownout mitigation: join WiFi FIRST, while the LCD backlight and audio
     * amp are still off. WiFi's first-time RF calibration is the biggest current
     * spike at boot; isolating it from the backlight + amp keeps the 3.3V rail
     * from sagging into a brownout reset loop on a weak USB supply. WiFi is
     * non-fatal — a bad AP shouldn't brick the local peripherals. */
    bool wifi_ok = (wifi_connect() == ESP_OK);
    if (!wifi_ok) {
        ESP_LOGW(TAG, "WiFi not connected; continuing offline");
    }

    /* Phase 1b: bring up the display, then touch on the shared I2C bus. The
     * backlight comes on at the end of display_init (after the WiFi spike). */
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(display_init(&i2c_bus));
    ESP_ERROR_CHECK(touch_init(i2c_bus));

    /* Phase 1c: audio playback. audio_play_init() always runs (needed for TTS
     * replies); the boot chime is opt-in — its amp current spike, on top of the
     * backlight + WiFi bring-up, can brown out a weak USB supply. */
    ESP_ERROR_CHECK(audio_play_init());
#if CONFIG_CLAWLEXA_BOOT_CHIME
    ESP_ERROR_CHECK(audio_play_wav(boot_wav_start,
                                   (size_t)(boot_wav_end - boot_wav_start)));
#endif

    /* Phase 1c-b: mic. Always bring it up; only auto-dump a recording over
     * serial (for tools/capture_mic.py) when explicitly built for it, since the
     * dump floods the console on every boot. */
    ESP_ERROR_CHECK(mic_init());
#if CONFIG_CLAWLEXA_MIC_DUMP_ON_BOOT
    ESP_ERROR_CHECK(mic_capture_and_dump(3));
#endif

    /* Phase 2a/2b: dial the bridge over WebSocket (requires WiFi above). */
    if (wifi_ok) {
        if (ws_connect() == ESP_OK) {
#if CONFIG_CLAWLEXA_STREAM_MIC_ON_CONNECT
            /* Continuously stream the mic; the task waits for the handshake and
             * re-opens the session on reconnect (no boot-time timing race). */
            ws_stream_mic_start();
#endif
        } else {
            ESP_LOGW(TAG, "bridge link not started");
        }
    }
#endif

    /* Phase 1a: heartbeat only. Audio bring-up lands in 1c. */
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t tick = 0;
    while (1) {
        ESP_LOGI(TAG, "heartbeat %lu", (unsigned long)tick++);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(5000));
    }
}

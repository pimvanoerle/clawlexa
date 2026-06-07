#include "touch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"

#include "board.h"
#include "touch_geom.h"

static const char *TAG = "touch";

#define PANEL_CENTER    (BOARD_LCD_H_RES / 2)
#define PANEL_RADIUS    (BOARD_LCD_H_RES / 2)
#define TOUCH_RELEASE_POLL_MS 25      /* while a finger is down, poll this often to catch the release */
#define TAP_DEBOUNCE_US (250 * 1000)  /* guard: a flaky mid-press blip must not re-trigger a tap */

static esp_lcd_touch_handle_t s_touch;
static SemaphoreHandle_t s_touch_int;  /* given by the INT ISR, taken by the poll task */
static void (*s_tap_cb)(void);         /* fired once per press (set by touch_set_tap_callback) */

void touch_set_tap_callback(void (*cb)(void)) {
    s_tap_cb = cb;
}

/* The CST816 INT (active-low) fired — runs in ISR context. Just wake the task;
 * all I2C reads happen there. Waking on the edge (instead of polling the INT
 * level every 50 ms) is what makes a quick single tap register reliably: a brief
 * INT pulse landing between two polls used to be missed, so a light tap dropped
 * and only a firm/long press would catch a poll. */
static void IRAM_ATTR touch_isr(esp_lcd_touch_handle_t tp) {
    (void)tp;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_touch_int, &hp);
    portYIELD_FROM_ISR(hp);  /* yields only if a higher-prio task was woken */
}

/* Fire the tap callback once per press inside the round active area. Idle: block
 * on the INT (no I2C — the CST816 NACKs reads in standby, which floods the log).
 * Touched: poll quickly to spot the release (the chip may not signal finger-up),
 * so the next press is seen as a fresh rising edge. */
static void touch_task(void *arg) {
    (void)arg;
    bool was_touching = false;
    int64_t last_tap_us = -TAP_DEBOUNCE_US;
    while (1) {
        TickType_t wait = was_touching ? pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS)
                                       : portMAX_DELAY;
        xSemaphoreTake(s_touch_int, wait);

        bool touching = false;
        esp_lcd_touch_point_data_t point = {0};
        uint8_t count = 0;
        esp_lcd_touch_read_data(s_touch);
        if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) == ESP_OK && count > 0 &&
            touch_in_circle((int16_t)point.x, (int16_t)point.y, PANEL_CENTER,
                            PANEL_CENTER, PANEL_RADIUS)) {
            touching = true;
            int64_t now = esp_timer_get_time();
            if (!was_touching && now - last_tap_us > TAP_DEBOUNCE_US) {
                last_tap_us = now;
                ESP_LOGI(TAG, "tap (%u, %u)", point.x, point.y);
                if (s_tap_cb != NULL) {
                    s_tap_cb();
                }
            }
        }
        was_touching = touching;
    }
}

esp_err_t touch_init(i2c_master_bus_handle_t i2c_bus) {
    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io),
                        TAG, "touch IO init failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = -1,  /* reset shares the PCA9554; released at display init */
        .int_gpio_num = BOARD_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,  /* active-low: the driver sets the INT GPIO to neg-edge */
        },
    };
    /* The CST816 NACKs I2C reads when idle/in standby, so its boot ID-read can
     * fail intermittently — retry a few times before giving up. */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_touch);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "CST816 init attempt %d failed (%s); retrying",
                 attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_RETURN_ON_ERROR(err, TAG, "CST816 init failed");

    s_touch_int = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_touch_int != NULL, ESP_ERR_NO_MEM, TAG,
                        "touch semaphore create failed");

    /* Register the INT before the task so no first-touch edge is lost (the ISR
     * just signals the semaphore the task waits on). */
    ESP_RETURN_ON_ERROR(esp_lcd_touch_register_interrupt_callback(s_touch, touch_isr),
                        TAG, "touch INT register failed");

    ESP_RETURN_ON_FALSE(
        xTaskCreate(touch_task, "touch", 3072, NULL, 4, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "touch task create failed");

    ESP_LOGI(TAG, "CST816 ready");
    return ESP_OK;
}

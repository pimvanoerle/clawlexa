#include "touch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"

#include "board.h"
#include "touch_geom.h"

static const char *TAG = "touch";

#define TOUCH_POLL_MS   50
#define PANEL_CENTER    (BOARD_LCD_H_RES / 2)
#define PANEL_RADIUS    (BOARD_LCD_H_RES / 2)
#define TOUCH_INT_ACTIVE_LEVEL  0  /* CST816 INT is active-low (tp_cfg.levels.interrupt) */

static esp_lcd_touch_handle_t s_touch;
static void (*s_tap_cb)(void);  /* fired once per press (set by touch_set_tap_callback) */

void touch_set_tap_callback(void (*cb)(void)) {
    s_tap_cb = cb;
}

/* Poll the controller and fire the tap callback on each new press inside the
 * round active area. The read is gated on the INT pin (the CST816 NACKs I2C
 * reads while idle, which would flood the log); a rising edge (no-touch ->
 * touch) debounces a held finger down to a single tap. */
static void touch_task(void *arg) {
    (void)arg;
    bool was_touching = false;
    while (1) {
        bool touching = false;
        if (touch_int_active(gpio_get_level(BOARD_TOUCH_INT_GPIO),
                             TOUCH_INT_ACTIVE_LEVEL)) {
            esp_lcd_touch_point_data_t point = {0};
            uint8_t count = 0;
            esp_lcd_touch_read_data(s_touch);
            if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) == ESP_OK && count > 0 &&
                touch_in_circle((int16_t)point.x, (int16_t)point.y, PANEL_CENTER,
                                PANEL_CENTER, PANEL_RADIUS)) {
                touching = true;
                if (!was_touching) {  /* rising edge = one tap */
                    ESP_LOGI(TAG, "tap (%u, %u)", point.x, point.y);
                    if (s_tap_cb != NULL) {
                        s_tap_cb();
                    }
                }
            }
        }
        was_touching = touching;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
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
            .interrupt = 0,
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

    ESP_RETURN_ON_FALSE(
        xTaskCreate(touch_task, "touch", 3072, NULL, 4, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "touch task create failed");

    ESP_LOGI(TAG, "CST816 ready");
    return ESP_OK;
}

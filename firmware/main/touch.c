#include "touch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static esp_lcd_touch_handle_t s_touch;

/* Poll the controller and log taps that land within the round active area. */
static void touch_task(void *arg) {
    (void)arg;
    while (1) {
        esp_lcd_touch_point_data_t point = {0};
        uint8_t count = 0;
        esp_lcd_touch_read_data(s_touch);
        if (esp_lcd_touch_get_data(s_touch, &point, &count, 1) == ESP_OK && count > 0) {
            if (touch_in_circle((int16_t)point.x, (int16_t)point.y, PANEL_CENTER,
                                PANEL_CENTER, PANEL_RADIUS)) {
                ESP_LOGI(TAG, "(%u, %u)", point.x, point.y);
            }
        }
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
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_touch),
                        TAG, "CST816 init failed");

    ESP_RETURN_ON_FALSE(
        xTaskCreate(touch_task, "touch", 3072, NULL, 4, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "touch task create failed");

    ESP_LOGI(TAG, "CST816 ready");
    return ESP_OK;
}

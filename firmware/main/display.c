#include "display.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "st77916_waveshare_init.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board.h"

static const char *TAG = "display";

#define LCD_HOST            SPI2_HOST
#define LCD_BITS_PER_PIXEL  16
/* Largest single QSPI transfer: a strip of ~80 lines at 2 bytes/px. */
#define LCD_MAX_TRANSFER_SZ (BOARD_LCD_H_RES * 80 * sizeof(uint16_t))
/* LVGL partial draw buffer: 40 lines, double-buffered (see lvgl_port). */
#define LCD_DRAW_BUF_LINES  40
/* QSPI read opcode (private in the component); used to probe panel register. */
#define LCD_OPCODE_READ_CMD (0x0BULL)
/* Slow clock for the one register read; full clock for pixel traffic. */
#define LCD_PROBE_CLK_HZ    (3 * 1000 * 1000)
#define LCD_PCLK_HZ         (40 * 1000 * 1000)

/* Bring up the shared I2C master bus (touch + IO expander sit on it). */
static esp_err_t init_i2c(i2c_master_bus_handle_t *out_bus) {
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, out_bus);
}

/* The LCD reset line hangs off PCA9554 pin 2, not an MCU GPIO. Bring the
 * expander up and pulse reset active-low, then release. */
static esp_err_t reset_lcd_via_expander(i2c_master_bus_handle_t i2c_bus) {
    esp_io_expander_handle_t io_expander = NULL;
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(
                            i2c_bus, BOARD_IO_EXPANDER_ADDR, &io_expander),
                        TAG, "PCA9554 init failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_2,
                                                IO_EXPANDER_OUTPUT),
                        TAG, "expander set_dir failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2, 0),
                        TAG, "expander reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2, 1),
                        TAG, "expander reset release failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

/* This board ships two ST77916 panel variants needing different init
 * sequences; Waveshare's demo tells them apart by reading register 0x04. We
 * replicate that: probe over a slow IO, then pick the matching array. */
static const st77916_lcd_init_cmd_t *select_init_cmds(esp_lcd_panel_io_handle_t io,
                                                      uint16_t *out_size) {
    uint8_t id[4] = {0};
    const int cmd = (int)((LCD_OPCODE_READ_CMD << 24) | (0x04 << 8));
    const esp_err_t err = esp_lcd_panel_io_rx_param(io, cmd, id, sizeof(id));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel reg 0x04 read failed (%s); assuming default variant",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "panel reg 0x04 = %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
    }

    if (err == ESP_OK && id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        ESP_LOGI(TAG, "panel variant: new");
        *out_size = sizeof(st77916_init_new) / sizeof(st77916_init_new[0]);
        return st77916_init_new;
    }
    ESP_LOGI(TAG, "panel variant: default");
    *out_size = sizeof(st77916_init_default) / sizeof(st77916_init_default[0]);
    return st77916_init_default;
}

/* Install the ST77916 over QSPI. Reset is handled externally (above), so the
 * panel driver gets reset_gpio_num = -1. */
static esp_err_t init_panel(esp_lcd_panel_io_handle_t *out_io,
                            esp_lcd_panel_handle_t *out_panel) {
    const spi_bus_config_t buscfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        BOARD_LCD_QSPI_CLK_GPIO, BOARD_LCD_QSPI_D0_GPIO, BOARD_LCD_QSPI_D1_GPIO,
        BOARD_LCD_QSPI_D2_GPIO, BOARD_LCD_QSPI_D3_GPIO, LCD_MAX_TRANSFER_SZ);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "QSPI bus init failed");

    esp_lcd_panel_io_spi_config_t io_config =
        ST77916_PANEL_IO_QSPI_CONFIG(BOARD_LCD_QSPI_CS_GPIO, NULL, NULL);

    /* Probe the panel variant on a slow IO, then tear it down. */
    io_config.pclk_hz = LCD_PROBE_CLK_HZ;
    esp_lcd_panel_io_handle_t probe_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &probe_io),
                        TAG, "probe IO init failed");
    uint16_t init_cmds_size = 0;
    const st77916_lcd_init_cmd_t *init_cmds = select_init_cmds(probe_io, &init_cmds_size);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(probe_io), TAG, "probe IO del failed");

    /* Real IO at full pixel clock. */
    io_config.pclk_hz = LCD_PCLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, out_io),
                        TAG, "panel IO init failed");

    const st77916_vendor_config_t vendor_config = {
        .init_cmds = init_cmds,
        .init_cmds_size = init_cmds_size,
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,  /* reset is driven via the PCA9554 expander */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(*out_io, &panel_config, out_panel),
                        TAG, "ST77916 init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*out_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*out_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*out_panel, true),
                        TAG, "panel display-on failed");
    return ESP_OK;
}

/* Clear the panel's power-on GRAM to black. LVGL's partial buffer only paints
 * dirty regions, so without this the undrawn background shows RAM garbage. */
static esp_err_t clear_panel_black(esp_lcd_panel_handle_t panel) {
    const size_t strip_px = BOARD_LCD_H_RES * LCD_DRAW_BUF_LINES;
    uint16_t *strip = heap_caps_calloc(strip_px, sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(strip != NULL, ESP_ERR_NO_MEM, TAG, "clear buffer alloc failed");

    esp_err_t err = ESP_OK;
    for (int y = 0; y < BOARD_LCD_V_RES && err == ESP_OK; y += LCD_DRAW_BUF_LINES) {
        int y_end = y + LCD_DRAW_BUF_LINES;
        if (y_end > BOARD_LCD_V_RES) {
            y_end = BOARD_LCD_V_RES;
        }
        err = esp_lcd_panel_draw_bitmap(panel, 0, y, BOARD_LCD_H_RES, y_end, strip);
    }
    vTaskDelay(pdMS_TO_TICKS(50));  /* let the last DMA transfer finish before free */
    free(strip);
    return err;
}

static void backlight_on(void) {
    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(BOARD_LCD_BL_GPIO, 1);
}

/* Hand the panel to LVGL and draw a centered "hello". */
static esp_err_t init_lvgl_and_hello(esp_lcd_panel_io_handle_t io_handle,
                                     esp_lcd_panel_handle_t panel_handle) {
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = BOARD_LCD_H_RES * LCD_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,  /* SPI panel expects byte-swapped RGB565 */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp failed");

    /* All LVGL calls must hold the port lock. */
    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_display_get_screen_active(disp);
        /* Pure-black background. NB: this panel shows very faint mura banding in
         * its center region on pure black only — confirmed cosmetic (a solid
         * color fill is perfectly clean), so not worth chasing in firmware. */
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label, "hello");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        lvgl_port_unlock();
    }
    return ESP_OK;
}

esp_err_t display_init(i2c_master_bus_handle_t *out_i2c_bus) {
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(init_i2c(&i2c_bus), TAG, "I2C bus init failed");
    ESP_RETURN_ON_ERROR(reset_lcd_via_expander(i2c_bus), TAG, "LCD reset failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_RETURN_ON_ERROR(init_panel(&io_handle, &panel_handle), TAG, "panel bring-up failed");
    ESP_RETURN_ON_ERROR(clear_panel_black(panel_handle), TAG, "panel clear failed");

    ESP_RETURN_ON_ERROR(init_lvgl_and_hello(io_handle, panel_handle), TAG, "LVGL bring-up failed");
    backlight_on();

    if (out_i2c_bus) {
        *out_i2c_bus = i2c_bus;
    }
    ESP_LOGI(TAG, "ST77916 ready");
    return ESP_OK;
}

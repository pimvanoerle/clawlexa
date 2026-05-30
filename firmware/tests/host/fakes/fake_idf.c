#include "fake_idf.h"

#include <string.h>

/* ---- recording state ------------------------------------------------------ */
#define FAKE_MAX_EVENTS 256
static const char *s_events[FAKE_MAX_EVENTS];
static int s_event_count;

uint8_t      faked_reg04[4];
bool         faked_reg04_fail;
int          faked_panel_reset_gpio;
uint16_t     faked_init_cmds_size;
const void  *faked_init_cmds;
int          faked_touch_int_gpio;
int          faked_touch_rst_gpio;
uint16_t     faked_touch_x_max;

void faked_reset(void) {
    s_event_count = 0;
    memset(s_events, 0, sizeof(s_events));
    faked_reg04[0] = 0; faked_reg04[1] = 0; faked_reg04[2] = 0; faked_reg04[3] = 0;
    faked_reg04_fail = false;
    faked_panel_reset_gpio = 0x7FFFFFFF;  /* sentinel: "not captured" */
    faked_init_cmds_size = 0;
    faked_init_cmds = NULL;
    faked_touch_int_gpio = 0x7FFFFFFF;
    faked_touch_rst_gpio = 0x7FFFFFFF;
    faked_touch_x_max = 0;
}

void faked_record(const char *event) {
    if (s_event_count < FAKE_MAX_EVENTS) {
        s_events[s_event_count++] = event;
    }
}

int faked_count(void) { return s_event_count; }

const char *faked_event(int i) {
    return (i >= 0 && i < s_event_count) ? s_events[i] : NULL;
}

int faked_index_of(const char *event) {
    for (int i = 0; i < s_event_count; i++) {
        if (strcmp(s_events[i], event) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- esp_err -------------------------------------------------------------- */
const char *esp_err_to_name(esp_err_t err) { (void)err; return "FAKE_ERR"; }

/* ---- handles: hand back non-NULL sentinels -------------------------------- */
static int s_i2c, s_panel_io, s_panel, s_expander, s_disp, s_obj, s_touch, s_task;

/* ---- freertos ------------------------------------------------------------- */
void vTaskDelay(TickType_t ticks) { (void)ticks; }

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, unsigned int prio, TaskHandle_t *out) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    faked_record("xTaskCreate");
    if (out) {
        *out = &s_task;
    }
    return pdPASS;  /* deliberately do not run fn (no polling loop in tests) */
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg,
                             i2c_master_bus_handle_t *ret) {
    (void)cfg;
    faked_record("i2c_new_master_bus");
    *ret = (i2c_master_bus_handle_t)&s_i2c;
    return ESP_OK;
}

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg,
                             spi_dma_chan_t dma) {
    (void)host; (void)cfg; (void)dma;
    faked_record("spi_bus_initialize");
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *cfg) {
    (void)cfg;
    faked_record("gpio_config");
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
    (void)pin; (void)level;
    faked_record("gpio_set_level");
    return ESP_OK;
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    faked_record("heap_caps_calloc");
    return calloc(n, size);
}

/* ---- panel IO / panel ----------------------------------------------------- */
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t *cfg,
                                   esp_lcd_panel_io_handle_t *ret) {
    (void)bus; (void)cfg;
    faked_record("panel_io_spi_new");
    *ret = (esp_lcd_panel_io_handle_t)&s_panel_io;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int cmd,
                                    void *param, size_t size) {
    (void)io; (void)cmd;
    faked_record("panel_io_rx_param");
    if (faked_reg04_fail) {
        return ESP_FAIL;
    }
    memcpy(param, faked_reg04, size < sizeof(faked_reg04) ? size : sizeof(faked_reg04));
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) {
    (void)io;
    faked_record("panel_io_del");
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t bus,
                                   const esp_lcd_panel_io_i2c_config_t *cfg,
                                   esp_lcd_panel_io_handle_t *ret) {
    (void)bus; (void)cfg;
    faked_record("panel_io_i2c_new");
    *ret = (esp_lcd_panel_io_handle_t)&s_panel_io;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_st77916(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *cfg,
                                    esp_lcd_panel_handle_t *ret) {
    (void)io;
    faked_record("st77916_new");
    faked_panel_reset_gpio = cfg->reset_gpio_num;
    const st77916_vendor_config_t *vc = cfg->vendor_config;
    if (vc) {
        faked_init_cmds = vc->init_cmds;
        faked_init_cmds_size = vc->init_cmds_size;
    }
    *ret = (esp_lcd_panel_handle_t)&s_panel;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p) {
    (void)p; faked_record("panel_reset"); return ESP_OK;
}
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) {
    (void)p; faked_record("panel_init"); return ESP_OK;
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) {
    (void)p; (void)on; faked_record("panel_disp_on"); return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0,
                                    int x1, int y1, const void *data) {
    (void)p; (void)x0; (void)y0; (void)x1; (void)y1; (void)data;
    faked_record("panel_draw_bitmap");
    return ESP_OK;
}

/* ---- esp_lcd_touch (cst816s) ---------------------------------------------- */
esp_err_t esp_lcd_touch_new_i2c_cst816s(esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_touch_config_t *cfg,
                                        esp_lcd_touch_handle_t *ret) {
    (void)io;
    faked_record("cst816s_new");
    faked_touch_int_gpio = cfg->int_gpio_num;
    faked_touch_rst_gpio = cfg->rst_gpio_num;
    faked_touch_x_max = cfg->x_max;
    *ret = (esp_lcd_touch_handle_t)&s_touch;
    return ESP_OK;
}

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp) {
    (void)tp;
    return ESP_OK;
}

esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp,
                                 esp_lcd_touch_point_data_t *data,
                                 uint8_t *cnt, uint8_t max_cnt) {
    (void)tp; (void)data; (void)max_cnt;
    *cnt = 0;  /* no touch points in the harness */
    return ESP_OK;
}

/* ---- io expander ---------------------------------------------------------- */
esp_err_t esp_io_expander_new_i2c_tca9554(i2c_master_bus_handle_t bus,
                                          uint32_t addr,
                                          esp_io_expander_handle_t *ret) {
    (void)bus; (void)addr;
    faked_record("expander_new");
    *ret = (esp_io_expander_handle_t)&s_expander;
    return ESP_OK;
}

esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t h, uint32_t pins,
                                  esp_io_expander_dir_t dir) {
    (void)h; (void)pins; (void)dir;
    faked_record("expander_set_dir");
    return ESP_OK;
}

esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t h, uint32_t pins,
                                    uint8_t level) {
    (void)h; (void)pins;
    faked_record(level ? "expander_reset_release" : "expander_reset_assert");
    return ESP_OK;
}

/* ---- lvgl + lvgl_port ----------------------------------------------------- */
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg) {
    (void)cfg; faked_record("lvgl_port_init"); return ESP_OK;
}
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *cfg) {
    (void)cfg; faked_record("lvgl_port_add_disp");
    return (lv_display_t *)&s_disp;
}
bool lvgl_port_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
void lvgl_port_unlock(void) {}

lv_obj_t *lv_display_get_screen_active(lv_display_t *disp) {
    (void)disp; return (lv_obj_t *)&s_obj;
}
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t s) {
    (void)o; (void)c; (void)s;
}
void lv_obj_set_style_text_color(lv_obj_t *o, lv_color_t c, lv_style_selector_t s) {
    (void)o; (void)c; (void)s;
}
lv_obj_t *lv_label_create(lv_obj_t *parent) { (void)parent; return (lv_obj_t *)&s_obj; }
void lv_label_set_text(lv_obj_t *label, const char *text) { (void)label; (void)text; }
void lv_obj_center(lv_obj_t *o) { (void)o; }
lv_color_t lv_color_black(void) { lv_color_t c = {0}; return c; }
lv_color_t lv_color_white(void) { lv_color_t c = {0xFFFFFF}; return c; }
lv_color_t lv_color_hex(uint32_t v) { lv_color_t c = {v}; return c; }

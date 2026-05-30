#pragma once

/* Minimal fakes for the ESP-IDF + component APIs that the firmware IO modules
 * call, so those modules can be compiled and exercised on the host. The fakes
 * record the sequence/arguments of calls; tests assert on that recording to
 * catch ordering/argument footguns (tier T3, see task_ideas.md) with no board.
 *
 * Only the subset the firmware actually uses is faked. Extend as new modules
 * (touch, audio) need more surface. The path-stub headers under fakes/ (e.g.
 * driver/i2c_master.h) all just include this file. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- esp_err -------------------------------------------------------------- */
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        (-1)
#define ESP_ERR_NO_MEM  0x101
const char *esp_err_to_name(esp_err_t err);

/* ---- call recording / test controls -------------------------------------- */
/* Reset the recording and all configurable behavior to defaults. */
void faked_reset(void);
/* Append an event to the call log. */
void faked_record(const char *event);
int  faked_count(void);
const char *faked_event(int i);
/* Index of the first recorded event equal to `event`, or -1 if absent. */
int  faked_index_of(const char *event);

/* Configurable behavior + captured arguments for assertions. */
extern uint8_t      faked_reg04[4];       /* bytes esp_lcd_panel_io_rx_param returns */
extern bool         faked_reg04_fail;     /* if true, rx_param returns ESP_FAIL */
extern int          faked_panel_reset_gpio;   /* captured reset_gpio_num */
extern uint16_t     faked_init_cmds_size;     /* captured vendor init_cmds_size */
extern const void  *faked_init_cmds;          /* captured vendor init_cmds ptr */

/* ---- freertos ------------------------------------------------------------- */
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void vTaskDelay(TickType_t ticks);

/* ---- driver/i2c_master ---------------------------------------------------- */
typedef struct fake_i2c_bus *i2c_master_bus_handle_t;
typedef enum { I2C_CLK_SRC_DEFAULT = 0 } i2c_clock_source_t;
#define I2C_NUM_0 0
typedef struct {
    int i2c_port;
    int sda_io_num;
    int scl_io_num;
    i2c_clock_source_t clk_source;
    int glitch_ignore_cnt;
    struct { unsigned enable_internal_pullup : 1; } flags;
} i2c_master_bus_config_t;
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg,
                             i2c_master_bus_handle_t *ret);

/* ---- driver/spi_master ---------------------------------------------------- */
typedef enum { SPI2_HOST = 1 } spi_host_device_t;
typedef enum { SPI_DMA_CH_AUTO = 3 } spi_dma_chan_t;
typedef struct {
    int sclk_io_num, mosi_io_num, miso_io_num, quadhd_io_num, quadwp_io_num;
    int data0_io_num, data1_io_num, data2_io_num, data3_io_num;
    int data4_io_num, data5_io_num, data6_io_num, data7_io_num;
    size_t max_transfer_sz;
    uint32_t flags;
    int intr_flags;
} spi_bus_config_t;
esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg,
                             spi_dma_chan_t dma);

/* ---- driver/gpio ---------------------------------------------------------- */
typedef int gpio_num_t;
typedef enum { GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    int pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);

/* ---- esp_heap_caps -------------------------------------------------------- */
#define MALLOC_CAP_DMA (1 << 3)
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);

/* ---- LCD common / panel io / ops / vendor --------------------------------- */
typedef enum {
    LCD_RGB_ELEMENT_ORDER_RGB = 0,
    LCD_RGB_ELEMENT_ORDER_BGR = 1,
} lcd_rgb_element_order_t;

typedef struct fake_panel_io *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_spi_bus_handle_t;
typedef struct {
    int cs_gpio_num, dc_gpio_num, spi_mode;
    unsigned pclk_hz;
    int trans_queue_depth;
    void *on_color_trans_done, *user_ctx;
    int lcd_cmd_bits, lcd_param_bits;
    struct {
        unsigned dc_low_on_data : 1, octal_mode : 1, quad_mode : 1;
        unsigned sio_mode : 1, lsb_first : 1, cs_high_active : 1;
    } flags;
} esp_lcd_panel_io_spi_config_t;
esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t *cfg,
                                   esp_lcd_panel_io_handle_t *ret);
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int cmd,
                                    void *param, size_t size);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);

typedef struct fake_panel *esp_lcd_panel_handle_t;
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t p);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0,
                                    int x1, int y1, const void *data);

typedef struct {
    int reset_gpio_num;
    lcd_rgb_element_order_t rgb_ele_order;
    int bits_per_pixel;
    struct { unsigned reset_active_high : 1; } flags;
    void *vendor_config;
} esp_lcd_panel_dev_config_t;

/* ---- esp_lcd_st77916 ------------------------------------------------------ */
typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} st77916_lcd_init_cmd_t;
typedef struct {
    const st77916_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct { unsigned use_mipi_interface : 1, use_qspi_interface : 1; } flags;
} st77916_vendor_config_t;
esp_err_t esp_lcd_new_panel_st77916(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *cfg,
                                    esp_lcd_panel_handle_t *ret);
#define ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, sz) { \
        .sclk_io_num = (sclk), .data0_io_num = (d0), .data1_io_num = (d1), \
        .data2_io_num = (d2), .data3_io_num = (d3), .max_transfer_sz = (sz) }
#define ST77916_PANEL_IO_QSPI_CONFIG(cs, cb, ctx) { \
        .cs_gpio_num = (cs), .dc_gpio_num = -1, .spi_mode = 0, \
        .pclk_hz = 40 * 1000 * 1000, .trans_queue_depth = 10, \
        .on_color_trans_done = (cb), .user_ctx = (ctx), \
        .lcd_cmd_bits = 32, .lcd_param_bits = 8, .flags = { .quad_mode = true } }

/* ---- esp_io_expander (+ tca9554) ------------------------------------------ */
typedef struct fake_io_expander *esp_io_expander_handle_t;
typedef enum { IO_EXPANDER_INPUT = 0, IO_EXPANDER_OUTPUT = 1 } esp_io_expander_dir_t;
enum {
    IO_EXPANDER_PIN_NUM_0 = (1U << 0), IO_EXPANDER_PIN_NUM_1 = (1U << 1),
    IO_EXPANDER_PIN_NUM_2 = (1U << 2), IO_EXPANDER_PIN_NUM_3 = (1U << 3),
};
esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t h, uint32_t pins,
                                  esp_io_expander_dir_t dir);
esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t h, uint32_t pins,
                                    uint8_t level);
#define ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000 0x20
esp_err_t esp_io_expander_new_i2c_tca9554(i2c_master_bus_handle_t bus,
                                          uint32_t addr,
                                          esp_io_expander_handle_t *ret);

/* ---- lvgl (minimal) ------------------------------------------------------- */
typedef struct fake_lv_disp lv_display_t;
typedef struct fake_lv_obj lv_obj_t;
typedef struct { uint32_t v; } lv_color_t;
typedef int lv_style_selector_t;
enum { LV_COLOR_FORMAT_RGB565 = 0x12 };
lv_obj_t *lv_display_get_screen_active(lv_display_t *disp);
void lv_obj_set_style_bg_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t s);
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t s);
lv_obj_t *lv_label_create(lv_obj_t *parent);
void lv_label_set_text(lv_obj_t *label, const char *text);
void lv_obj_center(lv_obj_t *obj);
lv_color_t lv_color_black(void);
lv_color_t lv_color_white(void);
lv_color_t lv_color_hex(uint32_t c);

/* ---- esp_lvgl_port -------------------------------------------------------- */
typedef struct {
    int task_priority, task_stack, task_affinity, task_max_sleep_ms;
    int task_stack_caps, timer_period_ms;
} lvgl_port_cfg_t;
#define ESP_LVGL_PORT_INIT_CONFIG() { \
        .task_priority = 4, .task_stack = 7168, .task_affinity = -1, \
        .task_max_sleep_ms = 500, .task_stack_caps = 0, .timer_period_ms = 5 }
typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle, control_handle;
    uint32_t buffer_size;
    bool double_buffer;
    uint32_t trans_size, hres, vres;
    bool monochrome;
    struct { bool swap_xy, mirror_x, mirror_y; } rotation;
    void *rounder_cb;
    int color_format;
    struct {
        unsigned buff_dma : 1, buff_spiram : 1, sw_rotate : 1;
        unsigned swap_bytes : 1, full_refresh : 1, direct_mode : 1;
    } flags;
} lvgl_port_display_cfg_t;
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg);
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *cfg);
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

/* ---- esp_log / esp_check (no-op logging, return-on-error control flow) ----- */
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, ...) do { (void)(tag); } while (0)

#define ESP_RETURN_ON_ERROR(x, tag, ...) do { \
        esp_err_t err_rc_ = (x); \
        if (err_rc_ != ESP_OK) { (void)(tag); return err_rc_; } \
    } while (0)
#define ESP_RETURN_ON_FALSE(a, err_code, tag, ...) do { \
        if (!(a)) { (void)(tag); return (err_code); } \
    } while (0)
#define ESP_ERROR_CHECK(x) do { esp_err_t err_rc_ = (x); (void)err_rc_; } while (0)

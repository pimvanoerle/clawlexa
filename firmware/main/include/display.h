#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

/* Phase 1b display bring-up for the ST77916 QSPI round panel.
 *
 * display_init() owns the shared I2C master bus (touch lives on it too), brings
 * up the PCA9554 IO expander, releases the expander-gated LCD reset, starts the
 * QSPI panel + LVGL, and shows "hello". The created I2C bus is handed back via
 * out_i2c_bus so touch_init() can reuse it. Logs "display: ST77916 ready". */
esp_err_t display_init(i2c_master_bus_handle_t *out_i2c_bus);

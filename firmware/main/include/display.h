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

/* Phase 6 (SPEC §8): agent-driven screen, fed by the bridge's set_state/show
 * MCP tools. display_set_state() shows the ambient status word in a per-state
 * color ("idle"/"listening"/"thinking"/"speaking"/"error"); display_show()
 * shows an arbitrary short line. Both are no-ops until display_init() ran, and
 * take the LVGL port lock internally — safe to call from any task. */
void display_set_state(const char *state);
void display_show(const char *text);

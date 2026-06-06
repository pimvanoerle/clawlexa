#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

/* Phase 1b touch bring-up. Initializes the CST816 capacitive controller on the
 * shared I2C bus and starts a task that logs tap coordinates (filtered to the
 * round panel) to serial. Report-only for now — no UI reaction; touch drives
 * the UI in Phase 6. Logs "touch: CST816 ready". */
esp_err_t touch_init(i2c_master_bus_handle_t i2c_bus);

/* Register a callback fired once per tap inside the round panel (Phase 6 touch
 * UI). Kept as a callback so touch.c stays decoupled from the WebSocket layer;
 * main wires this to ws_on_tap (tap-to-talk). Pass NULL to clear. */
void touch_set_tap_callback(void (*cb)(void));

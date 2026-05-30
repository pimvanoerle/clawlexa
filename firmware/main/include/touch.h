#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

/* Phase 1b touch bring-up. Initializes the CST816 capacitive controller on the
 * shared I2C bus and starts a task that logs tap coordinates (filtered to the
 * round panel) to serial. Report-only for now — no UI reaction; touch drives
 * the UI in Phase 6. Logs "touch: CST816 ready". */
esp_err_t touch_init(i2c_master_bus_handle_t i2c_bus);

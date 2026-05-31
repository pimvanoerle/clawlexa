#pragma once

#include "esp_err.h"

/* Phase 2a: connect to WiFi in station mode using the Kconfig SSID/password.
 * Blocks until connected (logs the IP) or times out. Returns ESP_OK on a
 * successful connection, ESP_ERR_INVALID_STATE if no SSID is configured, or
 * ESP_FAIL on timeout. Logs "wifi: connected, ip=...". */
esp_err_t wifi_connect(void);

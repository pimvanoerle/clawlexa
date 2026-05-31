#pragma once

#include "esp_err.h"

/* Phase 2b: connect to the bridge over WebSocket (ws://BRIDGE_HOST:BRIDGE_PORT
 * from Kconfig) and send a hello. Non-blocking — the client connects in the
 * background; the handshake is logged from its event handler. Returns
 * ESP_ERR_INVALID_STATE if no bridge host is configured. Requires WiFi.
 * Logs "ws: connected" and "ws: <- {welcome...}". */
esp_err_t ws_connect(void);

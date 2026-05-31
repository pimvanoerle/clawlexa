#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Phase 2b: connect to the bridge over WebSocket (ws://BRIDGE_HOST:BRIDGE_PORT
 * from Kconfig) and send a hello. Non-blocking — the client connects in the
 * background; the handshake is logged from its event handler. Returns
 * ESP_ERR_INVALID_STATE if no bridge host is configured. Requires WiFi.
 * Logs "ws: connected" and "ws: <- {welcome...}". */
esp_err_t ws_connect(void);

/* True once the WebSocket handshake with the bridge has completed. */
bool ws_is_connected(void);

/* Stream `seconds` of mic audio to the bridge as an audio_begin -> binary PCM
 * frames -> audio_end session (blocking). Requires ws_connect + mic_init. */
esp_err_t ws_stream_mic(uint32_t seconds);

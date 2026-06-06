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

/* Start a background task that continuously streams mic audio to the bridge:
 * sends audio_begin on each (re)connection, then 16-bit PCM frames. The bridge
 * endpoints utterances itself (server-side VAD). Half-duplex — while the bridge
 * is playing a reply (play_begin..play_end, plus a short tail) the mic is muted
 * so we don't capture our own speaker output. Requires ws_connect + mic_init. */
void ws_stream_mic_start(void);

/* Touch-driven push-to-talk (Phase 6). A screen tap calls this: when idle it
 * opens a streaming turn just like the wake word; mid-turn it ends the turn
 * (cancel). Safe to call from the touch task. */
void ws_on_tap(void);

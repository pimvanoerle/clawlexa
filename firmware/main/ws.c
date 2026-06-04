#include "ws.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

#include "app_version.h"
#include "audio.h"
#include "mic.h"

static const char *TAG = "ws";

#define WS_TEXT_OPCODE   0x01
#define WS_BIN_OPCODE    0x02
#define WS_CONT_OPCODE   0x00
#define MIC_STREAM_RATE 16000
#define MIC_FRAME_SAMPLES 256   /* 16 ms per binary frame @ 16 kHz */

static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected");
        s_connected = true;
        char hello[96];
        int len = snprintf(hello, sizeof(hello),
                           "{\"type\":\"hello\",\"device\":\"clawlexa\",\"fw\":\"%s\"}",
                           app_version());
        esp_websocket_client_send_text(s_client, hello, len, portMAX_DELAY);
        ESP_LOGI(TAG, "-> hello");
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        /* Text = control (e.g. the bridge's welcome); binary = PCM audio to play.
         * Continuation frames (0x00) carry the rest of a fragmented binary msg. */
        if (e->op_code == WS_TEXT_OPCODE && e->data_len > 0) {
            ESP_LOGI(TAG, "<- %.*s", e->data_len, (const char *)e->data_ptr);
        } else if ((e->op_code == WS_BIN_OPCODE || e->op_code == WS_CONT_OPCODE) &&
                   e->data_len >= 2) {
            audio_play_pcm((const int16_t *)e->data_ptr, e->data_len / 2);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        s_connected = false;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "error");
        s_connected = false;
        break;
    default:
        break;
    }
}

esp_err_t ws_connect(void) {
    ESP_RETURN_ON_FALSE(strlen(CONFIG_CLAWLEXA_BRIDGE_HOST) > 0, ESP_ERR_INVALID_STATE,
                        TAG, "no bridge host configured (set it via menuconfig)");

    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d", CONFIG_CLAWLEXA_BRIDGE_HOST,
             CONFIG_CLAWLEXA_BRIDGE_PORT);

    const esp_websocket_client_config_t cfg = { .uri = uri };
    s_client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_FAIL, TAG, "ws client init failed");
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                      on_ws_event, NULL),
                        TAG, "ws event register failed");

    ESP_LOGI(TAG, "connecting to %s", uri);
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(s_client), TAG, "ws start failed");
    return ESP_OK;
}

bool ws_is_connected(void) {
    return s_connected;
}

esp_err_t ws_stream_mic(uint32_t seconds) {
    ESP_RETURN_ON_FALSE(s_connected, ESP_ERR_INVALID_STATE, TAG, "not connected");

    /* Frame the recording: begin (control) -> binary PCM frames -> end. */
    const char *begin = "{\"type\":\"audio_begin\",\"rate\":16000,\"channels\":1,\"bits\":16}";
    esp_websocket_client_send_text(s_client, begin, strlen(begin), portMAX_DELAY);
    ESP_LOGI(TAG, "streaming %us of mic to bridge", (unsigned)seconds);

    int16_t buf[MIC_FRAME_SAMPLES];
    const uint32_t total = MIC_STREAM_RATE * seconds;
    for (uint32_t done = 0; done < total && s_connected; ) {
        int want = (int)(total - done) < MIC_FRAME_SAMPLES ? (int)(total - done)
                                                           : MIC_FRAME_SAMPLES;
        int got = mic_read_samples(buf, want);
        if (got <= 0) {
            ESP_LOGW(TAG, "mic read failed; stopping stream");
            break;
        }
        if (esp_websocket_client_send_bin(s_client, (const char *)buf,
                                          got * (int)sizeof(int16_t), portMAX_DELAY) < 0) {
            ESP_LOGW(TAG, "ws send failed; stopping stream");
            break;
        }
        done += got;
    }

    const char *end = "{\"type\":\"audio_end\"}";
    esp_websocket_client_send_text(s_client, end, strlen(end), portMAX_DELAY);
    ESP_LOGI(TAG, "stream done");
    return ESP_OK;
}

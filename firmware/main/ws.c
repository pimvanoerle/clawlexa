#include "ws.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

#include "app_version.h"
#include "audio.h"
#include "mic.h"
#include "mic_gate.h"

static const char *TAG = "ws";

#define WS_TEXT_OPCODE   0x01
#define WS_BIN_OPCODE    0x02
#define WS_CONT_OPCODE   0x00
#define MIC_FRAME_SAMPLES 256   /* 16 ms per binary frame @ 16 kHz */
#define MIC_PLAYBACK_TAIL_US 300000  /* keep mic muted 300 ms past playback */

static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;
static volatile int64_t s_mute_until_us;  /* half-duplex: mute mic while we speak */

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
            /* Half-duplex: a play_begin..play_end pair brackets the bridge's
             * spoken reply — mute the mic across it (+ a tail) so we don't
             * capture and re-stream our own speaker output. */
            char ctrl[96];
            int n = e->data_len < (int)sizeof(ctrl) - 1 ? e->data_len
                                                        : (int)sizeof(ctrl) - 1;
            memcpy(ctrl, e->data_ptr, n);
            ctrl[n] = '\0';
            s_mute_until_us = mic_gate_update(s_mute_until_us, ctrl,
                                              esp_timer_get_time(),
                                              MIC_PLAYBACK_TAIL_US);
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

/* Continuously stream the mic to the bridge. Opens a fresh audio session
 * (audio_begin) on each (re)connection, then forwards 16-bit PCM frames. The
 * bridge endpoints utterances with its own VAD, so there is no per-utterance
 * audio_end — the stream is open for the life of the connection. */
static void mic_stream_task(void *arg) {
    (void)arg;
    int16_t buf[MIC_FRAME_SAMPLES];
    bool streaming = false;
    while (1) {
        if (!s_connected) {
            streaming = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!streaming) {
            const char *begin =
                "{\"type\":\"audio_begin\",\"rate\":16000,\"channels\":1,\"bits\":16}";
            esp_websocket_client_send_text(s_client, begin, strlen(begin), portMAX_DELAY);
            ESP_LOGI(TAG, "mic stream started");
            streaming = true;
        }
        int got = mic_read_samples(buf, MIC_FRAME_SAMPLES);
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        /* Half-duplex: keep draining the mic DMA while muted, but don't forward
         * our own speaker output back to the bridge. */
        if (mic_gate_muted(s_mute_until_us, esp_timer_get_time())) {
            continue;
        }
        if (esp_websocket_client_send_bin(s_client, (const char *)buf,
                                          got * (int)sizeof(int16_t), portMAX_DELAY) < 0) {
            ESP_LOGW(TAG, "mic send failed");
        }
    }
}

void ws_stream_mic_start(void) {
    xTaskCreate(mic_stream_task, "mic_stream", 4096, NULL, 5, NULL);
}

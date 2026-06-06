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
#include "display.h"
#include "mic.h"
#include "mic_gate.h"
#include "wake_detector.h"
#include "wake_gate.h"

static const char *TAG = "ws";

#define WS_TEXT_OPCODE   0x01
#define WS_BIN_OPCODE    0x02
#define WS_CONT_OPCODE   0x00
#define MIC_FRAME_SAMPLES 256   /* 16 ms per binary frame @ 16 kHz */
#define MIC_PLAYBACK_TAIL_US 300000  /* keep mic muted 300 ms past playback */
#define WAKE_TURN_TIMEOUT_US (10 * 1000000)  /* end a turn if the bridge never replies */

static esp_websocket_client_handle_t s_client;
static volatile bool s_connected;
static volatile int64_t s_mute_until_us;  /* half-duplex: mute mic while we speak */
static volatile bool s_turn_end;  /* set on play_end: the bridge's reply finished */

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
            /* play_end means the reply finished — close the wake-triggered turn. */
            if (strstr(ctrl, "play_end") != NULL) {
                s_turn_end = true;
            }
            /* Agent-driven display (Phase 6): set_state -> status word/color,
             * show -> arbitrary line. (ctrl is the frame truncated to 95 bytes;
             * long show text is clipped — fine for the small round screen.) */
            if (strstr(ctrl, "\"set_state\"") != NULL) {
                static const char *const states[] = {"idle", "listening",
                                                     "thinking", "speaking", "error"};
                for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
                    char needle[16];
                    snprintf(needle, sizeof(needle), "\"%s\"", states[i]);
                    if (strstr(ctrl, needle) != NULL) {
                        display_set_state(states[i]);
                        break;
                    }
                }
            } else if (strstr(ctrl, "\"show\"") != NULL) {
                const char *t = strstr(ctrl, "\"text\"");
                if (t != NULL && (t = strchr(t + 6, '"')) != NULL) {
                    t++;  /* opening quote of the value */
                    const char *end = strchr(t, '"');
                    if (end != NULL) {
                        char msg[80];
                        int len = (int)(end - t);
                        if (len > (int)sizeof(msg) - 1) {
                            len = (int)sizeof(msg) - 1;
                        }
                        memcpy(msg, t, (size_t)len);
                        msg[len] = '\0';
                        display_show(msg);
                    }
                }
            }
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

/* Wake-gated mic loop (Phase 4b). In LISTENING the mic only feeds the on-device
 * wake detector — nothing leaves the device. When the wake word fires, open a
 * streaming turn (audio_begin -> PCM frames) so the bridge transcribes the
 * command; the turn ends — audio_end, back to LISTENING — when the bridge's
 * reply finishes (play_end) or a no-reply timeout elapses. Half-duplex: while
 * our own reply plays we neither stream nor listen (mic_gate covers the clip). */
static void mic_stream_task(void *arg) {
    (void)arg;
    if (!wake_detector_init()) {
        ESP_LOGE(TAG, "wake detector init failed; mic will stay silent");
    }
    int16_t buf[MIC_FRAME_SAMPLES];
    wake_state_t state = WAKE_LISTENING;
    int64_t turn_deadline = 0;
    while (1) {
        int got = mic_read_samples(buf, MIC_FRAME_SAMPLES);
        if (got <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        int64_t now = esp_timer_get_time();

        if (state == WAKE_LISTENING) {
            /* Don't react to our own reply still draining from the speaker. */
            if (mic_gate_muted(s_mute_until_us, now)) {
                continue;
            }
            if (wake_detector_feed(buf, (size_t) got) && s_connected) {
                ESP_LOGI(TAG, "wake -> streaming a turn");
                state = wake_gate_next(state, WAKE_EV_WAKE);
                s_turn_end = false;
                const char *begin =
                    "{\"type\":\"audio_begin\",\"rate\":16000,\"channels\":1,\"bits\":16}";
                esp_websocket_client_send_text(s_client, begin, strlen(begin), portMAX_DELAY);
                turn_deadline = now + WAKE_TURN_TIMEOUT_US;
            }
            continue;
        }

        /* WAKE_STREAMING: forward the command to the bridge. */
        if (!s_connected) {  /* lost the link mid-turn */
            state = wake_gate_next(state, WAKE_EV_TURN_END);
            continue;
        }
        if (!mic_gate_muted(s_mute_until_us, now)) {  /* half-duplex */
            if (esp_websocket_client_send_bin(s_client, (const char *)buf,
                                              got * (int)sizeof(int16_t), portMAX_DELAY) < 0) {
                ESP_LOGW(TAG, "mic send failed");
            }
        }
        if (s_turn_end || now > turn_deadline) {
            ESP_LOGI(TAG, "turn end -> listening");
            const char *end = "{\"type\":\"audio_end\"}";
            esp_websocket_client_send_text(s_client, end, strlen(end), portMAX_DELAY);
            state = wake_gate_next(state, WAKE_EV_TURN_END);
        }
    }
}

void ws_stream_mic_start(void) {
    /* 8 KB stack — the wake detector runs TFLite-Micro inference in this task. */
    xTaskCreate(mic_stream_task, "mic_stream", 8192, NULL, 5, NULL);
}

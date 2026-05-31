#include "ws.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

#include "app_version.h"

static const char *TAG = "ws";

#define WS_TEXT_OPCODE 0x01

static esp_websocket_client_handle_t s_client;

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected");
        char hello[96];
        int len = snprintf(hello, sizeof(hello),
                           "{\"type\":\"hello\",\"device\":\"clawlexa\",\"fw\":\"%s\"}",
                           app_version());
        esp_websocket_client_send_text(s_client, hello, len, portMAX_DELAY);
        ESP_LOGI(TAG, "-> hello");
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        /* Control frames are text; log them (e.g. the bridge's welcome). */
        if (e->op_code == WS_TEXT_OPCODE && e->data_len > 0) {
            ESP_LOGI(TAG, "<- %.*s", e->data_len, (const char *)e->data_ptr);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "error");
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

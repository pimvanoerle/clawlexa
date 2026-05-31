#include "mic.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include "mic_sample.h"
#include "board.h"

static const char *TAG = "mic";

#define MIC_RATE      16000
#define READ_CHUNK    256   /* raw int32 samples per I2S read */
#define B64_RAW_LINE  48    /* raw bytes per base64 line (multiple of 3) */

static i2s_chan_handle_t s_rx;

/* RX-only master I2S for the ICS-43434. The mic is 24-bit; we read 32-bit slots
 * (mono left) and convert in mic_sample_to_pcm16. No MCLK, no I2C. */
esp_err_t mic_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s_new_channel failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_MIC_I2S_SCK_GPIO,
            .ws   = BOARD_MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = BOARD_MIC_I2S_SD_GPIO,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s enable failed");

    ESP_LOGI(TAG, "ICS-43434 ready");
    return ESP_OK;
}

int mic_read_samples(int16_t *out, int max_samples) {
    if (s_rx == NULL || max_samples <= 0) {
        return -1;
    }
    int32_t raw[READ_CHUNK];
    int want = max_samples < READ_CHUNK ? max_samples : READ_CHUNK;
    size_t br = 0;
    if (i2s_channel_read(s_rx, raw, want * sizeof(int32_t), &br, pdMS_TO_TICKS(1000)) != ESP_OK) {
        return -1;
    }
    int got = (int)(br / sizeof(int32_t));
    for (int i = 0; i < got; i++) {
        out[i] = mic_sample_to_pcm16(raw[i]);
    }
    return got;
}

esp_err_t mic_capture_and_dump(uint32_t seconds) {
    ESP_RETURN_ON_FALSE(s_rx, ESP_ERR_INVALID_STATE, TAG, "mic not initialized");

    const size_t n_samples = (size_t)MIC_RATE * seconds;
    int16_t *pcm = heap_caps_malloc(n_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(pcm, ESP_ERR_NO_MEM, TAG, "pcm buffer alloc failed");

    /* Discard ~50 ms while the mic settles after enable. */
    int32_t raw[READ_CHUNK];
    size_t junk = 0;
    for (int i = 0; i < 4; i++) {
        i2s_channel_read(s_rx, raw, sizeof(raw), &junk, pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "capturing %us...", (unsigned)seconds);
    size_t got = 0;
    while (got < n_samples) {
        size_t want = (n_samples - got < READ_CHUNK) ? (n_samples - got) : READ_CHUNK;
        size_t br = 0;
        if (i2s_channel_read(s_rx, raw, want * sizeof(int32_t), &br, pdMS_TO_TICKS(1000)) != ESP_OK) {
            break;
        }
        size_t samples = br / sizeof(int32_t);
        for (size_t i = 0; i < samples; i++) {
            pcm[got + i] = mic_sample_to_pcm16(raw[i]);
        }
        got += samples;
    }

    /* Dump as base64, framed, for tools/capture_mic.py. Each line encodes a
     * multiple of 3 raw bytes so the concatenation decodes as one stream. */
    const uint8_t *bytes = (const uint8_t *)pcm;
    const size_t total = got * sizeof(int16_t);
    printf("MIC_WAV_BEGIN rate=%d ch=1 bits=16 bytes=%u\n", MIC_RATE, (unsigned)total);
    unsigned char line[B64_RAW_LINE * 4 / 3 + 4];
    for (size_t off = 0; off < total; off += B64_RAW_LINE) {
        size_t chunk = (total - off < B64_RAW_LINE) ? (total - off) : B64_RAW_LINE;
        size_t olen = 0;
        mbedtls_base64_encode(line, sizeof(line), &olen, bytes + off, chunk);
        printf("%.*s\n", (int)olen, line);
    }
    printf("MIC_WAV_END\n");

    free(pcm);
    ESP_LOGI(TAG, "capture done (%u samples)", (unsigned)got);
    return ESP_OK;
}

#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#include "audio_tone.h"
#include "wav.h"
#include "board.h"

static const char *TAG = "audio";

#define AUDIO_SAMPLE_RATE  16000
#define TONE_AMPLITUDE     8000
#define TONE_CHUNK         320  /* 20 ms @ 16 kHz mono */

static i2s_chan_handle_t s_tx;

/* The speaker is a PCM5101A I2S DAC into an NS8002 amplifier — no I2C control
 * codec, and the DAC runs MCLK-less (internal PLL off BCLK). So playback is
 * just a master I2S TX channel; write PCM and it comes out the speaker. */
esp_err_t audio_play_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* On TX underrun send silence, not a repeat of the last DMA buffer —
     * otherwise a finite tone/clip drones on continuously. */
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  /* PCM5101A derives its clock from BCLK */
            .bclk = BOARD_SPK_I2S_BCLK_GPIO,
            .ws   = BOARD_SPK_I2S_WS_GPIO,
            .dout = BOARD_SPK_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable failed");

    ESP_LOGI(TAG, "PCM5101 ready");
    return ESP_OK;
}

esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    ESP_RETURN_ON_FALSE(s_tx, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    const uint32_t total = (uint32_t)((uint64_t)AUDIO_SAMPLE_RATE * duration_ms / 1000);
    int16_t buf[TONE_CHUNK];
    ESP_LOGI(TAG, "playing %luHz for %lums", (unsigned long)freq_hz,
             (unsigned long)duration_ms);

    for (uint32_t done = 0; done < total; ) {
        uint32_t n = (total - done < TONE_CHUNK) ? (total - done) : TONE_CHUNK;
        audio_fill_sine(buf, n, freq_hz, AUDIO_SAMPLE_RATE, TONE_AMPLITUDE, done);
        size_t written = 0;
        ESP_RETURN_ON_ERROR(
            i2s_channel_write(s_tx, buf, n * sizeof(int16_t), &written, portMAX_DELAY),
            TAG, "i2s write failed");
        done += n;
    }
    return ESP_OK;
}

esp_err_t audio_play_wav(const uint8_t *wav, size_t len) {
    ESP_RETURN_ON_FALSE(s_tx, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    wav_info_t info;
    ESP_RETURN_ON_FALSE(wav_parse(wav, len, &info), ESP_ERR_INVALID_ARG, TAG, "bad WAV");
    ESP_RETURN_ON_FALSE(info.bits_per_sample == 16 && info.channels == 1,
                        ESP_ERR_NOT_SUPPORTED, TAG, "need 16-bit mono WAV");
    ESP_LOGI(TAG, "playing WAV: %luHz, %lu bytes",
             (unsigned long)info.sample_rate, (unsigned long)info.data_bytes);

    /* Retune the I2S clock to the WAV's sample rate (channel must be idle). */
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "i2s disable failed");
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(info.sample_rate);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG, "i2s reclock failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable failed");

    size_t written = 0;
    ESP_RETURN_ON_ERROR(
        i2s_channel_write(s_tx, info.data, info.data_bytes, &written, portMAX_DELAY),
        TAG, "i2s write failed");
    return ESP_OK;
}

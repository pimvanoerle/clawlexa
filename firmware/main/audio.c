#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

#include "audio_tone.h"
#include "board.h"

static const char *TAG = "audio";

#define AUDIO_SAMPLE_RATE  16000
#define AUDIO_BITS         16
#define AUDIO_CHANNELS     1
#define TONE_AMPLITUDE     8000
#define TONE_CHUNK         320  /* 20 ms @ 16 kHz mono */
#define PLAYER_VOLUME      70

static esp_codec_dev_handle_t s_play_dev;

/* Master-mode I2S TX for playback. Reset is N/A; MCLK is required by ES8311. */
static esp_err_t init_i2s_tx(i2s_chan_handle_t *out_tx) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, out_tx, NULL), TAG, "i2s_new_channel failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws   = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*out_tx, &std_cfg), TAG, "i2s std init failed");
    return ESP_OK;
}

esp_err_t audio_play_init(i2c_master_bus_handle_t i2c_bus) {
    /* WIP (task_ideas AU-1): the ES8311 does not ACK on I2C on this board yet —
     * not powered/out-of-reset via a path we've identified (expander rails and
     * MCLK ruled out; needs the schematic). Probe first and bail with a single
     * warning so boot isn't flooded with codec I2C errors. Remove once the
     * codec reliably enumerates. */
    if (i2c_master_probe(i2c_bus, BOARD_ES8311_ADDR >> 1, 100) != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 (0x%02x) not responding; audio disabled (see AU-1)",
                 BOARD_ES8311_ADDR >> 1);
        return ESP_ERR_NOT_FOUND;
    }

    i2s_chan_handle_t tx = NULL;
    ESP_RETURN_ON_ERROR(init_i2s_tx(&tx), TAG, "I2S TX init failed");

    /* ES8311's I2C interface only responds while MCLK is running. esp_codec_dev
     * doesn't enable I2S until esp_codec_dev_open(), which is AFTER the codec's
     * register config — so we start MCLK ourselves first, then hand the channel
     * back (disable) so esp_codec_dev_open can re-enable it cleanly. */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s enable (for MCLK) failed");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = NULL };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data interface failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = BOARD_ES8311_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl interface failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "gpio interface failed");

    /* pa_pin lets the codec driver gate the speaker amp (GPIO15) on open/close —
     * miss this and the codec is happy but the speaker is silent. */
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_AUDIO_PA_GPIO,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311_codec_new failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_play_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_play_dev, ESP_FAIL, TAG, "esp_codec_dev_new failed");

    /* Codec configured; hand the I2S channel back so open() re-enables it. */
    ESP_RETURN_ON_ERROR(i2s_channel_disable(tx), TAG, "i2s disable failed");

    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_play_dev, PLAYER_VOLUME) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set volume failed");
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = AUDIO_BITS,
        .channel = AUDIO_CHANNELS,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_play_dev, &fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "codec open failed");

    ESP_LOGI(TAG, "ES8311 ready");
    return ESP_OK;
}

esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    ESP_RETURN_ON_FALSE(s_play_dev, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    const uint32_t total = (uint32_t)((uint64_t)AUDIO_SAMPLE_RATE * duration_ms / 1000);
    int16_t buf[TONE_CHUNK];
    ESP_LOGI(TAG, "playing %luHz for %lums", (unsigned long)freq_hz,
             (unsigned long)duration_ms);

    for (uint32_t done = 0; done < total; ) {
        uint32_t n = (total - done < TONE_CHUNK) ? (total - done) : TONE_CHUNK;
        audio_fill_sine(buf, n, freq_hz, AUDIO_SAMPLE_RATE, TONE_AMPLITUDE, done);
        ESP_RETURN_ON_FALSE(
            esp_codec_dev_write(s_play_dev, buf, (int)(n * sizeof(int16_t))) == ESP_CODEC_DEV_OK,
            ESP_FAIL, TAG, "codec write failed");
        done += n;
    }
    return ESP_OK;
}

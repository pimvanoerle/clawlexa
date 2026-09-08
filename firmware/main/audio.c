#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"

#include "audio_tone.h"
#include "pcm_ring.h"
#include "wav.h"
#include "board.h"

static const char *TAG = "audio";

#define AUDIO_SAMPLE_RATE  16000
#define TONE_AMPLITUDE     8000
#define TONE_CHUNK         320  /* 20 ms @ 16 kHz mono */

/* Jitter buffer for streamed playback. 2 s at 16 kHz = 64 KB, allocated from
 * PSRAM (CONFIG_SPIRAM) so it costs no internal RAM. The I2S DMA underneath is
 * only 90 ms deep, so this is what actually absorbs a WiFi stall. */
#define PLAY_RING_SAMPLES  (AUDIO_SAMPLE_RATE * 2)
/* Audio to have in hand before the first sample goes out. Cheap in practice:
 * the bridge pushes a whole clip far faster than real time, so this is ~30 ms
 * of wall clock, and it comfortably fits inside the device's post-playback mic
 * mute tail. */
#define PLAY_PREROLL_SAMPLES  (AUDIO_SAMPLE_RATE * 300 / 1000)
#define PLAY_CHUNK_SAMPLES 512
#define PLAY_FULL_WAIT_MS  10

static i2s_chan_handle_t s_tx;
static pcm_ring_t s_ring;
static volatile bool s_clip_open;    /* between play_begin and play_end */
static volatile bool s_draining;     /* a clip is queued or playing */
static audio_play_stats_t s_stats;
static int64_t s_clip_started_us;

static void audio_play_task(void *arg);

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

    /* Jitter buffer + the task that drains it (PSRAM: 64 KB, no internal RAM). */
    int16_t *storage = heap_caps_malloc(PLAY_RING_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM);
    if (storage == NULL) {  /* no PSRAM: fall back to a smaller internal buffer */
        ESP_LOGW(TAG, "no PSRAM for the playback buffer; using internal RAM");
        storage = heap_caps_malloc(PLAY_RING_SAMPLES / 4 * sizeof(int16_t),
                                   MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(storage, ESP_ERR_NO_MEM, TAG, "playback buffer alloc failed");
        pcm_ring_init(&s_ring, storage, PLAY_RING_SAMPLES / 4);
    } else {
        pcm_ring_init(&s_ring, storage, PLAY_RING_SAMPLES);
    }
    /* Above the WebSocket/mic tasks (5): if the DAC is starved everything else
     * can wait — this task only ever runs briefly to top up the DMA. */
    xTaskCreate(audio_play_task, "audio_play", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "PCM5101 ready (%lums playback buffer)",
             (unsigned long)(s_ring.cap * 1000 / AUDIO_SAMPLE_RATE));
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

void audio_play_begin(void) {
    pcm_ring_reset(&s_ring);
    s_stats = (audio_play_stats_t){0};
    s_clip_started_us = esp_timer_get_time();
    s_clip_open = true;
    s_draining = true;
}

void audio_play_end(void) {
    s_clip_open = false;  /* the task drains what is left, then reports */
}

audio_play_stats_t audio_play_get_stats(void) {
    return s_stats;
}

esp_err_t audio_play_pcm(const int16_t *samples, size_t n_samples) {
    ESP_RETURN_ON_FALSE(s_tx, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    if (!s_draining) {
        /* PCM without a play_begin (an older bridge, or a stray frame after
         * end): treat it as its own clip rather than dropping it. */
        audio_play_begin();
    }
    /* Back-pressure rather than drop: a short write means we are already ~2 s
     * ahead, which is exactly when it is safe to make the sender wait. */
    while (n_samples > 0) {
        size_t took = pcm_ring_write(&s_ring, samples, n_samples);
        size_t level = pcm_ring_level(&s_ring);
        if (level > s_stats.max_level) {
            s_stats.max_level = (uint32_t)level;
        }
        samples += took;
        n_samples -= took;
        if (n_samples > 0) {
            vTaskDelay(pdMS_TO_TICKS(PLAY_FULL_WAIT_MS));
        }
    }
    return ESP_OK;
}

/* Drains the ring into I2S. Runs on its own task so a network stall never
 * starves the DAC, and so the WebSocket event handler is no longer blocked
 * inside an i2s write for the length of a reply. */
static void audio_play_task(void *arg) {
    (void)arg;
    int16_t chunk[PLAY_CHUNK_SAMPLES];
    bool started = false;
    while (1) {
        if (!s_draining) {
            started = false;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t level = pcm_ring_level(&s_ring);
        if (!started) {
            /* Wait for a cushion — unless the clip is already complete and
             * shorter than the pre-roll, in which case just play it. */
            if (level < PLAY_PREROLL_SAMPLES && s_clip_open) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            started = true;
            s_stats.preroll_ms =
                (uint32_t)((esp_timer_get_time() - s_clip_started_us) / 1000);
        }
        size_t n = pcm_ring_read(&s_ring, chunk, PLAY_CHUNK_SAMPLES);
        if (n == 0) {
            if (!s_clip_open) {  /* fully sent and fully played */
                s_draining = false;
                s_stats.dropped = (uint32_t)s_ring.dropped;
                ESP_LOGI(TAG, "clip done: %lu samples, %lu underruns, "
                              "peak buffer %lums, preroll %lums, dropped %lu",
                         (unsigned long)s_stats.samples,
                         (unsigned long)s_stats.underruns,
                         (unsigned long)(s_stats.max_level * 1000 / AUDIO_SAMPLE_RATE),
                         (unsigned long)s_stats.preroll_ms,
                         (unsigned long)s_stats.dropped);
                continue;
            }
            /* Mid-clip and nothing to play: the DMA is draining toward silence.
             * This is the gap, counted. */
            s_stats.underruns++;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t written = 0;
        if (i2s_channel_write(s_tx, chunk, n * sizeof(int16_t), &written,
                              portMAX_DELAY) == ESP_OK) {
            s_stats.samples += (uint32_t)(written / sizeof(int16_t));
            if (written != n * sizeof(int16_t)) {
                ESP_LOGW(TAG, "short i2s write: %u of %u bytes",
                         (unsigned)written, (unsigned)(n * sizeof(int16_t)));
            }
        }
    }
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

#include "wake_detector.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "wake_models/audio_preprocessor_int8_model_data.h"  // g_audio_preprocessor_int8_tflite
#include "wake_stats.h"
#include "wake_window.h"

static const char *TAG = "wake";

/* --- microWakeWord pipeline geometry ------------------------------------- */
#define FEATURE_SIZE 40
#define SAMPLE_RATE 16000
#define WINDOW_SAMPLES 480   /* 30 ms feature window */
#define STRIDE_SAMPLES 160   /* 10 ms feature step (feature_step_size in manifests) */
/* Refractory: suppress detections until this many sub-cutoff slices have passed
 * (after load and after each detection), so one utterance fires once. */
#define MIN_SLICES_BEFORE_DETECTION 100
/* Every slice must finish inside the feature cadence or detection falls behind
 * real time. STRIDE_SAMPLES at 16 kHz = 10 ms, and that is the whole budget the
 * preprocessor and every streaming model share (SPEC §7, Phase 4c). */
#define SLICE_BUDGET_US   (STRIDE_SAMPLES * 1000000 / 16000)
/* How often to report. ~30 s at one slice per 10 ms — often enough to watch a
 * model being added, rare enough not to bury the log. */
#define STATS_EVERY_SLICES 3000

#define VAR_ARENA_SIZE 1024
#define PREPROCESSOR_ARENA_SIZE (16 * 1024)

/* Embedded streaming models (EMBED_FILES in main/CMakeLists.txt). */
extern const uint8_t okay_nabu_start[] asm("_binary_okay_nabu_tflite_start");
extern const uint8_t hey_pinchy_start[] asm("_binary_hey_pinchy_tflite_start");
extern const uint8_t vad_start[] asm("_binary_vad_tflite_start");

/* ====================== Wake word selection ==============================
 * To use a different wake word (e.g. a trained "clawlexa" or "okay iPinch"):
 *   1. Train a microWakeWord model — see training/README.md.
 *   2. Drop <word>.tflite into main/wake_models/ and add it to EMBED_FILES.
 *   3. Point the three macros below at it (symbol, label, and the manifest's
 *      probability_cutoff). The default is the zero-setup bring-up word.
 * The symbol is "_binary_<file>_tflite_start" for "<file>.tflite". */
/* The wake phrases this build listens for (SPEC §7, Phase 4c).
 *
 * microWakeWord is one model per phrase, so several phrases means several models
 * fed the same feature stream, OR-ed — each with the cutoff and window from its
 * OWN manifest. The cutoff genuinely differs per model (okay_nabu 0.97,
 * hey_pinchy 0.50); it is a property of how that model was trained and
 * benchmarked, not a strictness dial to be harmonised.
 *
 * CPU is what bounds the list: every model runs on every 10 ms slice. The load
 * meter reports the share used, so check it after adding one — measured at ~30%
 * for one phrase plus the VAD gate, and ~12% per additional phrase.
 *
 * To add a phrase: train it (training/README.md), drop the .tflite in
 * wake_models/, add it to EMBED_FILES, declare its symbol above, and add a row
 * here. The generic build ships only the zero-setup bring-up word so a clone
 * works out of the box without adopting anyone's pet name for their crab. */
struct WakePhrase {
    const uint8_t *model_start;
    const char *label;
    float cutoff;       /* probability_cutoff from the model's .json */
    int window;         /* sliding_window_size from the same manifest */
};

static const WakePhrase WAKE_PHRASES[] = {
    { okay_nabu_start,  "okay nabu",  0.97f, 5 },
    /* An iPinch build adds its own phrases here, e.g.:
     *   { hey_pinchy_start, "hey pinchy", 0.50f, 5 },
     * Measured on device: one phrase + VAD is ~30% of the 10 ms slice, two 42%. */
};
/* ========================================================================= */

namespace {

constexpr uint8_t quantize_cutoff(float cutoff) {
    int q = (int) (cutoff * 255.0f + 0.5f);
    return (uint8_t) std::min(std::max(q, 0), 255);
}

/* One streaming microWakeWord model (the wake word, or the VAD gate).
 * Feeds 40-feature int8 slices through a streaming INT8 model and tracks a
 * sliding window of probabilities. Mirrors ESPHome's StreamingModel. */
class StreamModel {
 public:
    StreamModel(const uint8_t *model_start, float cutoff, int window, size_t arena_size,
                const char *name = "model")
        : name_(name),
          model_start_(model_start),
          cutoff_(quantize_cutoff(cutoff)),
          window_(window),
          arena_size_(arena_size) {}

    bool load() {
        register_ops_();
        var_arena_ = (uint8_t *) heap_caps_malloc(VAR_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        /* Tensor arena in PSRAM — generous, keeps internal RAM for WiFi/LVGL. */
        tensor_arena_ = (uint8_t *) heap_caps_malloc(arena_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (var_arena_ == nullptr || tensor_arena_ == nullptr) {
            ESP_LOGE(TAG, "arena alloc failed (%u + %d bytes)", (unsigned) arena_size_, VAR_ARENA_SIZE);
            return false;
        }
        tflite::MicroAllocator *ma = tflite::MicroAllocator::Create(var_arena_, VAR_ARENA_SIZE);
        tflite::MicroResourceVariables *mrv = tflite::MicroResourceVariables::Create(ma, 20);

        const tflite::Model *model = tflite::GetModel(model_start_);
        if (model->version() != TFLITE_SCHEMA_VERSION) {
            ESP_LOGE(TAG, "streaming model schema mismatch");
            return false;
        }
        interp_ = std::make_unique<tflite::MicroInterpreter>(model, resolver_, tensor_arena_, arena_size_, mrv);
        if (interp_->AllocateTensors() != kTfLiteOk) {
            ESP_LOGE(TAG, "AllocateTensors failed (arena %u too small?)", (unsigned) arena_size_);
            return false;
        }
        stride_ = interp_->input(0)->dims->data[1];
        win_storage_.assign((size_t) window_, 0);
        wake_window_init(&win_, win_storage_.data(), window_, cutoff_,
                         MIN_SLICES_BEFORE_DETECTION);
        ESP_LOGI(TAG, "streaming model loaded: stride=%d arena_used=%u cutoff=%u win=%d",
                 stride_, (unsigned) interp_->arena_used_bytes(), cutoff_, window_);
        return true;
    }

    /* Feed one 40-feature int8 slice; sets have_new_ when a fresh probability
     * was produced (every `stride` slices the model buffers internally). */
    void infer(const int8_t feats[FEATURE_SIZE]) {
        TfLiteTensor *input = interp_->input(0);
        stride_step_ = stride_step_ % stride_;
        std::memmove(tflite::GetTensorData<int8_t>(input) + FEATURE_SIZE * stride_step_, feats, FEATURE_SIZE);
        ++stride_step_;
        if (stride_step_ >= stride_) {
            const int64_t t0 = esp_timer_get_time();
            if (interp_->Invoke() != kTfLiteOk) {
                ESP_LOGW(TAG, "invoke failed");
                return;
            }
            wake_stats_add(&stats_, (uint32_t)(esp_timer_get_time() - t0));
            wake_window_push(&win_, interp_->output(0)->data.uint8[0]);
            have_new_ = true;
        }
    }

    bool consume_new() { bool n = have_new_; have_new_ = false; return n; }

    /* The verdict lives in wake_window (pure, host-tested): a sliding-window
     * average over this model's own cutoff, plus a refractory period so one
     * spoken phrase fires once. Each phrase carries its own cutoff. */
    bool detected() const { return wake_window_fired(&win_); }
    bool active() const { return wake_window_active(&win_); }
    uint8_t latest() const { return wake_window_latest(&win_); }

    const char *name() const { return name_; }
    wake_stage_stats_t *stats() { return &stats_; }

    void reset() { wake_window_reset(&win_); }

 private:
    void register_ops_() {
        resolver_.AddCallOnce();        resolver_.AddVarHandle();
        resolver_.AddReshape();         resolver_.AddReadVariable();
        resolver_.AddStridedSlice();    resolver_.AddConcatenation();
        resolver_.AddAssignVariable();  resolver_.AddConv2D();
        resolver_.AddMul();             resolver_.AddAdd();
        resolver_.AddMean();            resolver_.AddFullyConnected();
        resolver_.AddLogistic();        resolver_.AddQuantize();
        resolver_.AddDepthwiseConv2D(); resolver_.AddAveragePool2D();
        resolver_.AddMaxPool2D();       resolver_.AddPad();
        resolver_.AddPack();            resolver_.AddSplitV();
    }

    const char *name_;
    wake_stage_stats_t stats_ = {};
    const uint8_t *model_start_;
    uint8_t cutoff_;
    int window_;
    size_t arena_size_;
    uint8_t *var_arena_ = nullptr;
    uint8_t *tensor_arena_ = nullptr;
    std::unique_ptr<tflite::MicroInterpreter> interp_;
    tflite::MicroMutableOpResolver<20> resolver_;
    int stride_ = 1;
    int stride_step_ = 0;
    std::vector<uint8_t> win_storage_;
    mutable wake_window_t win_ = {};
    bool have_new_ = false;
};

/* The audio preprocessor: a TFLite model (signal ops) that turns a 480-sample
 * (30 ms) int16 window into 40 int8 features — the modern micro_speech frontend.
 * We invoke it every 10 ms (STRIDE_SAMPLES) to match the streaming models. */
alignas(16) uint8_t g_prep_arena[PREPROCESSOR_ARENA_SIZE];
tflite::MicroMutableOpResolver<18> g_prep_resolver;
std::unique_ptr<tflite::MicroInterpreter> g_prep_interp;
std::vector<int16_t> g_samples;  // sample accumulator across feed() calls

/* One model per phrase, all fed the same slices. unique_ptr because
 * StreamModel owns an interpreter and its arenas. */
std::vector<std::unique_ptr<StreamModel>> g_wakes;
StreamModel g_vad(vad_start, 0.50f, 5, 96 * 1024, "vad");

/* Preprocessor cost and the slice counter the per-slice figures divide by. */
wake_stage_stats_t g_prep_stats = {};
uint32_t g_slices = 0;
bool g_ready = false;

bool init_preprocessor_() {
    g_prep_resolver.AddReshape();      g_prep_resolver.AddCast();
    g_prep_resolver.AddStridedSlice(); g_prep_resolver.AddConcatenation();
    g_prep_resolver.AddMul();          g_prep_resolver.AddAdd();
    g_prep_resolver.AddDiv();          g_prep_resolver.AddMinimum();
    g_prep_resolver.AddMaximum();      g_prep_resolver.AddWindow();
    g_prep_resolver.AddFftAutoScale(); g_prep_resolver.AddRfft();
    g_prep_resolver.AddEnergy();       g_prep_resolver.AddFilterBank();
    g_prep_resolver.AddFilterBankSquareRoot();
    g_prep_resolver.AddFilterBankSpectralSubtraction();
    g_prep_resolver.AddPCAN();         g_prep_resolver.AddFilterBankLog();

    const tflite::Model *model = tflite::GetModel(g_audio_preprocessor_int8_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "preprocessor model schema mismatch");
        return false;
    }
    g_prep_interp = std::make_unique<tflite::MicroInterpreter>(model, g_prep_resolver, g_prep_arena,
                                                               PREPROCESSOR_ARENA_SIZE);
    if (g_prep_interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "preprocessor AllocateTensors failed");
        return false;
    }
    return true;
}

/* Run the preprocessor over one 30 ms window into a 40-feature int8 slice. */
void generate_feature_(const int16_t *window, int8_t out[FEATURE_SIZE]) {
    TfLiteTensor *input = g_prep_interp->input(0);
    std::copy_n(window, WINDOW_SAMPLES, tflite::GetTensorData<int16_t>(input));
    const int64_t t0 = esp_timer_get_time();
    if (g_prep_interp->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "preprocessor invoke failed");
        return;
    }
    wake_stats_add(&g_prep_stats, (uint32_t)(esp_timer_get_time() - t0));
    std::copy_n(tflite::GetTensorData<int8_t>(g_prep_interp->output(0)), FEATURE_SIZE, out);
}

/* How much of the 10 ms slice the detector is using, and where it goes. This is
 * the number that decides how many wake phrases fit (Phase 4c): every model runs
 * on every slice, so the answer is (budget - current) / cost-per-model. Logged
 * per slice rather than per invoke — a model that invokes every 3rd slice is
 * cheap per slice however expensive each invoke is. */
void report_load_() {
    const uint32_t prep = wake_stats_per_slice_us(&g_prep_stats, g_slices);
    const uint32_t vad  = wake_stats_per_slice_us(g_vad.stats(), g_slices);
    uint32_t wake = 0;
    std::string per_word;
    for (auto &m : g_wakes) {
        const uint32_t us = wake_stats_per_slice_us(m->stats(), g_slices);
        wake += us;
        char buf[64];
        snprintf(buf, sizeof(buf), " | %s %luus (max %lu)", m->name(),
                 (unsigned long) us, (unsigned long) m->stats()->max_us);
        per_word += buf;
    }
    const uint32_t total = prep + wake + vad;
    ESP_LOGI(TAG,
             /* No %ll here: ESP-IDF builds with newlib's *nano* formatting, which
              * does not implement 64-bit specifiers. A stray %llu misparses the
              * varargs and the following %s then dereferences garbage — this
              * crashed the device (LoadProhibited) every time the first report
              * fired. Everything below fits in unsigned long anyway. */
             "load: %lu%% of %luus slice | prep %luus (max %lu)%s "
             "| vad %luus (max %lu) | headroom %luus",
             (unsigned long) wake_stats_budget_pct(total, SLICE_BUDGET_US),
             (unsigned long) SLICE_BUDGET_US,
             (unsigned long) prep, (unsigned long) g_prep_stats.max_us,
             per_word.c_str(),
             (unsigned long) vad, (unsigned long) g_vad.stats()->max_us,
             (unsigned long) (total < SLICE_BUDGET_US ? SLICE_BUDGET_US - total : 0));
    g_slices = 0;
    wake_stats_reset(&g_prep_stats);
    for (auto &m : g_wakes) {
        wake_stats_reset(m->stats());
    }
    wake_stats_reset(g_vad.stats());
}

}  // namespace

extern "C" bool wake_detector_init(void) {
    if (!init_preprocessor_()) return false;
    for (const WakePhrase &p : WAKE_PHRASES) {
        auto m = std::make_unique<StreamModel>(p.model_start, p.cutoff, p.window,
                                               96 * 1024, p.label);
        if (!m->load()) {
            ESP_LOGE(TAG, "failed to load wake model '%s'", p.label);
            return false;
        }
        g_wakes.push_back(std::move(m));
    }
    if (g_wakes.empty() || !g_vad.load()) return false;
    g_samples.reserve(WINDOW_SAMPLES * 4);
    g_ready = true;

    std::string words;
    for (const auto &m : g_wakes) {
        if (!words.empty()) words += ", ";
        words += m->name();
    }
    ESP_LOGI(TAG, "wake detector ready (%s + vad)", words.c_str());
    return true;
}

extern "C" bool wake_detector_feed(const int16_t *samples, size_t count) {
    if (!g_ready) return false;
    g_samples.insert(g_samples.end(), samples, samples + count);
    bool fired = false;
    size_t pos = 0;
    while (g_samples.size() - pos >= WINDOW_SAMPLES) {
        int8_t feats[FEATURE_SIZE];
        generate_feature_(g_samples.data() + pos, feats);
        for (auto &m : g_wakes) {
            m->infer(feats);
        }
        g_vad.infer(feats);
        /* Every phrase gets consume_new() called so its "fresh probability" flag
         * clears whether or not an earlier phrase already fired this slice. */
        for (auto &m : g_wakes) {
            if (!(m->consume_new() && m->detected())) {
                continue;
            }
            if (g_vad.active()) {
                ESP_LOGI(TAG, "WAKE: %s (p=%u)", m->name(), m->latest());
                fired = true;
                /* Reset them all: the phrases overlap acoustically, so a second
                 * one firing on the tail of the first would be the same
                 * utterance counted twice. */
                for (auto &other : g_wakes) {
                    other->reset();
                }
                break;
            }
            ESP_LOGD(TAG, "wake blocked by vad: %s (p=%u)", m->name(), m->latest());
            m->reset();
        }
        pos += STRIDE_SAMPLES;  // 10 ms hop
        if (++g_slices >= STATS_EVERY_SLICES) {
            report_load_();
        }
    }
    g_samples.erase(g_samples.begin(), g_samples.begin() + pos);  // keep the tail
    return fired;
}

#include "wake_detector.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "wake_models/audio_preprocessor_int8_model_data.h"  // g_audio_preprocessor_int8_tflite

static const char *TAG = "wake";

/* --- microWakeWord pipeline geometry ------------------------------------- */
#define FEATURE_SIZE 40
#define SAMPLE_RATE 16000
#define WINDOW_SAMPLES 480   /* 30 ms feature window */
#define STRIDE_SAMPLES 160   /* 10 ms feature step (feature_step_size in manifests) */
/* Refractory: suppress detections until this many sub-cutoff slices have passed
 * (after load and after each detection), so one utterance fires once. */
#define MIN_SLICES_BEFORE_DETECTION 100
#define VAR_ARENA_SIZE 1024
#define PREPROCESSOR_ARENA_SIZE (16 * 1024)

/* Embedded streaming models (EMBED_FILES in main/CMakeLists.txt). */
extern const uint8_t okay_nabu_start[] asm("_binary_okay_nabu_tflite_start");
extern const uint8_t vad_start[] asm("_binary_vad_tflite_start");

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
    StreamModel(const uint8_t *model_start, float cutoff, int window, size_t arena_size)
        : model_start_(model_start),
          cutoff_(quantize_cutoff(cutoff)),
          window_(window),
          arena_size_(arena_size) {}

    bool load() {
        register_ops_();
        var_arena_ = (uint8_t *) heap_caps_malloc(VAR_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        tensor_arena_ = (uint8_t *) heap_caps_malloc(arena_size_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (var_arena_ == nullptr || tensor_arena_ == nullptr) {
            ESP_LOGE(TAG, "arena alloc failed (%zu + %d bytes)", arena_size_, VAR_ARENA_SIZE);
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
            ESP_LOGE(TAG, "AllocateTensors failed (arena %zu too small?)", arena_size_);
            return false;
        }
        stride_ = interp_->input(0)->dims->data[1];
        recent_.assign(window_, 0);
        ignore_ = -MIN_SLICES_BEFORE_DETECTION;
        ESP_LOGI(TAG, "streaming model loaded: stride=%d arena=%zu cutoff=%u win=%d",
                 stride_, arena_size_, cutoff_, window_);
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
            if (interp_->Invoke() != kTfLiteOk) {
                ESP_LOGW(TAG, "invoke failed");
                return;
            }
            last_idx_ = (last_idx_ + 1) % window_;
            recent_[last_idx_] = interp_->output(0)->data.uint8[0];
            have_new_ = true;
        }
        if (recent_[last_idx_] < cutoff_) {
            ignore_ = std::min(ignore_ + 1, 0);  // cool-off climbs back to 0
        }
    }

    bool consume_new() { bool n = have_new_; have_new_ = false; return n; }

    /* Sliding-window average over cutoff, respecting the refractory window. */
    bool detected() const {
        if (ignore_ < 0) return false;
        return windowed_sum_() > (uint32_t) cutoff_ * window_;
    }

    /* VAD variant: no refractory, just the windowed average. */
    bool active() const { return windowed_sum_() > (uint32_t) cutoff_ * window_; }

    uint8_t latest() const { return recent_[last_idx_]; }

    void reset() {
        std::fill(recent_.begin(), recent_.end(), 0);
        ignore_ = -MIN_SLICES_BEFORE_DETECTION;
    }

 private:
    uint32_t windowed_sum_() const {
        uint32_t sum = 0;
        for (uint8_t p : recent_) sum += p;
        return sum;
    }

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
    std::vector<uint8_t> recent_;
    int last_idx_ = 0;
    int ignore_ = -MIN_SLICES_BEFORE_DETECTION;
    bool have_new_ = false;
};

/* The audio preprocessor: a TFLite model (signal ops) that turns a 480-sample
 * (30 ms) int16 window into 40 int8 features — the modern micro_speech frontend.
 * We invoke it every 10 ms (STRIDE_SAMPLES) to match the streaming models. */
alignas(16) uint8_t g_prep_arena[PREPROCESSOR_ARENA_SIZE];
tflite::MicroMutableOpResolver<18> g_prep_resolver;
std::unique_ptr<tflite::MicroInterpreter> g_prep_interp;
std::vector<int16_t> g_samples;  // sample accumulator across feed() calls

StreamModel g_wake(okay_nabu_start, 0.97f, 5, 28000);
StreamModel g_vad(vad_start, 0.50f, 5, 22000);
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
    if (g_prep_interp->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "preprocessor invoke failed");
        return;
    }
    std::copy_n(tflite::GetTensorData<int8_t>(g_prep_interp->output(0)), FEATURE_SIZE, out);
}

}  // namespace

extern "C" bool wake_detector_init(void) {
    if (!init_preprocessor_()) return false;
    if (!g_wake.load() || !g_vad.load()) return false;
    g_samples.reserve(WINDOW_SAMPLES * 4);
    g_ready = true;
    ESP_LOGI(TAG, "wake detector ready (okay_nabu + vad)");
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
        g_wake.infer(feats);
        g_vad.infer(feats);
        if (g_wake.consume_new() && g_wake.detected()) {
            if (g_vad.active()) {
                ESP_LOGI(TAG, "WAKE: okay nabu (p=%u)", g_wake.latest());
                fired = true;
            } else {
                ESP_LOGD(TAG, "wake blocked by vad (p=%u)", g_wake.latest());
            }
            g_wake.reset();
        }
        pos += STRIDE_SAMPLES;  // 10 ms hop
    }
    g_samples.erase(g_samples.begin(), g_samples.begin() + pos);  // keep the tail
    return fired;
}

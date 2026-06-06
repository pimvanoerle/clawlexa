# Wake-word models

On-device wake-word models for the microWakeWord pipeline (see
[`wake_detector.cc`](../wake_detector.cc)). These are **checked in** deliberately —
they're freely licensed, so the firmware builds with a working wake word and no
setup. To train your own word, see [`training/README.md`](../../../training/README.md).

| File | What it is | Source / license |
|------|------------|------------------|
| `okay_nabu.tflite` + `.json` | Wake model — the bring-up/default word "okay nabu" | [esphome/micro-wake-word-models][m] (v2), Apache-2.0 |
| `vad.tflite` + `.json` | Voice-activity gate (suppresses non-speech false triggers) | [esphome/micro-wake-word-models][m] (v2), Apache-2.0 |
| `audio_preprocessor_int8_model_data.h` | The 16 kHz→40-feature spectrogram front end (signal ops) | TFLite-Micro `micro_speech` example (esp-tflite-micro), Apache-2.0 |

The `.json` manifests carry each model's `probability_cutoff`, `sliding_window_size`,
and `feature_step_size` — `wake_detector.cc` mirrors these (the cutoff lives in the
wake-word selection block).

[m]: https://github.com/esphome/micro-wake-word-models

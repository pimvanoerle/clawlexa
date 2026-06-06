# Training a custom wake word

clawlexa detects its wake word **on-device** with [microWakeWord][mww] (open,
Apache-2.0), running a small streaming TFLite-Micro model on the ESP32-S3. The
generic build ships a working model (`okay nabu`) so you don't *need* to train
anything — but a custom word (e.g. **`clawlexa`**, or iPinch's **`okay iPinch`**)
needs its own model. This is how you mint one and drop it into the firmware.

> ⚠️ **Reality check (from upstream):** training a *good* wake-word model is
> hard and experimental — it takes iteration on the phrase, sample generation,
> and hyperparameters. Expect a few rounds. The `okay nabu` default is there so
> the device works out of the box while you tune your own.

The whole pipeline is **phrase-parameterized** — everywhere below, just change
the phrase and you have the recipe for `okay iPinch` instead of `clawlexa`.

## How it works (so the model matches our firmware)

microWakeWord models all use the **same fixed front end**, which is exactly what
the firmware runs (`firmware/main/wake_detector.cc` + the vendored
`audio_preprocessor_int8` model): 16 kHz mono → 40 features per 30 ms window,
every 10 ms. So **any** microWakeWord v2 model is compatible — you don't have to
match any settings; just train and drop it in.

## Step 1 — Train the model

Pick one (both are free, no account-with-company-email nonsense):

- **Easiest: the web trainer at [microwakeword.com][site].** Enter your phrase
  (`clawlexa`), let it generate samples + train, download the `.tflite` + its
  `.json` manifest.
- **More control: the Colab notebook** [`basic_training_notebook.ipynb`][nb] from
  the [OHF-Voice/micro-wake-word][mww] repo. It uses [Piper sample generator][psg]
  to synthesize many spoken variants of your phrase, augments them, pulls the
  pre-generated negative/ambient datasets from [HuggingFace][hf], trains on the
  Colab GPU, and exports a streaming `int8` `.tflite` + manifest. Set the wake
  phrase near the top of the notebook; the rest is parameterized off it.

Either way you end up with two files, e.g. `clawlexa.tflite` and
`clawlexa.json`. The manifest's `probability_cutoff` is the detection threshold —
note it (raise it if you get false triggers, lower it if it misses).

## Step 2 — Drop it into the firmware

1. Copy `clawlexa.tflite` into `firmware/main/wake_models/`.
2. Add it to `EMBED_FILES` in `firmware/main/CMakeLists.txt`:
   ```cmake
   EMBED_FILES
       "assets/boot.wav"
       "wake_models/okay_nabu.tflite"
       "wake_models/vad.tflite"
       "wake_models/clawlexa.tflite"     # <-- add
   ```
3. Point the **wake-word selection** block at it in
   `firmware/main/wake_detector.cc` (one place):
   ```c
   extern const uint8_t clawlexa_start[] asm("_binary_clawlexa_tflite_start");
   #define WAKE_MODEL_START   clawlexa_start
   #define WAKE_WORD_LABEL    "clawlexa"
   #define WAKE_CUTOFF        0.95f   /* = probability_cutoff from clawlexa.json */
   ```
   (The symbol is `_binary_<file>_tflite_start` for `<file>.tflite`.)
4. Rebuild + flash. Validate detection fast with the detect-only image:
   `idf.py menuconfig` → enable **clawlexa → Wake-word bring-up test**, flash,
   and watch the serial for `WAKE: clawlexa` while you say it.

## Per-agent words

The word is a **per-build asset**: the generic clawlexa build can use `clawlexa`,
while an iPinch build points the macros at an `okay_iPinch.tflite`. Same firmware,
different embedded model — no code changes beyond the selection block.

[mww]: https://github.com/OHF-Voice/micro-wake-word
[nb]: https://github.com/OHF-Voice/micro-wake-word/blob/main/notebooks/basic_training_notebook.ipynb
[psg]: https://github.com/rhasspy/piper-sample-generator
[hf]: https://huggingface.co/datasets/kahrendt/microwakeword
[site]: https://microwakeword.com

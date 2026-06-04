# Post-training task ideas

Running log of standalone exercises this repo can provide for evaluating /
post-training *other* models. See the rationale in
[CLAUDE.md](./CLAUDE.md#post-training-task-ideas-running-log).

Each entry: **anchor** (commit/PR to check out), **prompt** (what you'd hand
the model), **done-correctly** (the observable check a grader can run), and a
**tier** marking how it can be graded. Append-only; prune only when something
stops being true.

## Verification tiers

Most footguns split into a *decision/logic core* (gradeable with no board) and a
*physical effect* (needs the board). Tag each task with the tier(s) it can be
graded at, so a grader runs the slice that fits the environment. Tiers 1–4 run
headless in Docker (`espressif/idf` image); only tier 5 needs hardware.

- **T1 Build** — `idf.py build` for esp32s3 succeeds. Catches wrong/missing
  components, bad APIs, CMake/requires errors.
- **T2 Host-logic** — pure C unit tests (Unity, `tests/host/`). Catches decision
  logic: variant selection, geometry, parsers, state machines.
- **T3 Host-mock** — link firmware logic against fake IDF driver headers, assert
  call *sequence/arguments*. Catches ordering (reset-before-init,
  `reset_gpio_num=-1`, right init array, clear-before-LVGL). Harness lives in
  `tests/host/fakes/` (`fake_idf.h` + path-stubs + `fake_idf.c`); extend its
  surface as new IO modules need it.
- **T4 QEMU** — boot smoke on the **headless build** (`CONFIG_CLAWLEXA_HEADLESS=y`,
  see `firmware/sdkconfig.qemu`), which skips the LCD/touch init QEMU can't model
  and lets the banner + heartbeat run. Catches panics / early-log regressions for
  the logic-only boot path. Runs in the Docker CI image (it ships Espressif's
  QEMU fork + deps); **not runnable locally here** — mainline `qemu-system-xtensa`
  has no esp32 machine, and Espressif's fork needs `libgcrypt` which isn't
  installed. Wire the `pytest-embedded-qemu` test once running in that image.
- **T5 Device** — real board. The only tier that confirms actual pixels/taps.

When a task is T5-only at face value, decompose it: list the device-free slices
(T1–T4) separately so CI grades the parts a model most often gets wrong.

---

## Toolchain & hardware bring-up

### TI-1 — Get ESP-IDF installed on macOS from a cold machine
- **Tier:** T5 (host machine state; not Dockerable — it's about the dev's mac).
- **Anchor:** any commit with `firmware/` present (≥ Phase 1a).
- **Prompt:** "Install the ESP-IDF toolchain so `idf.py build` works in
  `firmware/`. The user is on macOS with a python.org Python."
- **Done-correctly:** model diagnoses the `CERTIFICATE_VERIFY_FAILED` footgun
  (python.org Python ships no wired-in CA bundle;
  `ssl.get_default_verify_paths().cafile` is `None`) and fixes it via
  `Install Certificates.command` *before* blindly retrying the download.
  Footgun: models that just re-run `install.sh` or `pip install certifi`
  without fixing the SSL default path loop forever.

### TI-2 — Flash and verify first boot
- **Tier:** T5 device (real boot) — but T4 QEMU can grade the `clawlexa booted`
  banner headless, since Phase 1a firmware is logic-only (no peripheral init).
- **Anchor:** Phase 1a tip.
- **Prompt:** "Build, flash to the board, and prove the firmware is running."
- **Done-correctly:** captures serial, finds `clawlexa booted` + the 5 s
  heartbeat cadence. Footgun: the board enumerates as a native USB-Serial-JTAG
  device; capturing the *boot banner* (not just heartbeats) requires pulsing
  the reset line, not just opening the port.

## Phase 1b — display + touch bring-up

### DT-1 — Add the right managed components and keep it building
- **Tier:** T1 (fully device-free — the whole task is "make it build").
- **Anchor:** the "docs/spec" commit (before deps are added).
- **Prompt:** "Add the ESP-IDF managed components needed to drive this board's
  ST77916 QSPI panel through its PCA9554 IO expander, with LVGL and CST816
  touch. Make `idf.py build` succeed."
- **Done-correctly:** correct registry components resolve and the tree still
  builds. Footgun: picking the non-C panel driver, the wrong IO-expander part
  (TCA vs PCA95xx), or an LVGL major version the port doesn't support.

### DT-2 — Bring up the panel (hello on screen)
- **Tier:** T5 for the visible `hello`; T1 (builds) + T3 (reset sequenced via
  expander before panel init, `reset_gpio_num=-1`) device-free.
- **Anchor:** the "add managed components" commit.
- **Prompt:** "Initialize the display and show `hello` centered on the round
  screen."
- **Done-correctly:** `display: ST77916 ready` in the log and a visible
  `hello`. Footgun: LCD reset is **not** a direct GPIO — it hangs off PCA9554
  pin 2. A model that passes a reset GPIO to the panel driver, or skips the
  expander entirely, gets a black screen.

### DT-3 — The two-panel-variant init footgun (the hard one)
- **Anchor:** "add managed components" commit (panel not yet working).
- **Prompt:** "Get a clean image on the screen — the panel currently shows
  scrambled stripes."
- **Done-correctly:** crisp, stable content with a clean background. This is a
  layered trap that mirrors a real bring-up session:
  1. The generic `esp_lcd_st77916` default init does NOT drive this panel —
     output is scrambled. You must supply Waveshare's vendor `init_cmds`.
  2. **There are TWO panel variants on this board**, needing *different* init
     arrays. Waveshare's demo picks between them by reading panel register
     `0x04` over a slow (3 MHz) IO before re-opening at full clock. A model that
     hardcodes one array has a ~50% chance of a still-scrambled panel. Correct
     answer replicates the register probe (our board reports `00 02 7f 7f` =
     "new" variant, which also needs INVON `0x21`).
  3. LVGL's partial draw buffer only paints dirty regions, so power-on GRAM
     garbage shows through the undrawn background until you explicitly clear the
     panel to black once.
- **Grader signal:** the boot log prints `panel reg 0x04 = ...` and
  `panel variant: <new|default>`; correct firmware selects to match the board.
- **Source of truth:** waveshareteam/ESP32-S3-Touch-LCD-1.85C, `main/LCD_Driver/`.
- **Note:** a residual very-faint mura band on pure black at screen center is a
  physical panel trait (a solid color fill is perfectly clean) — NOT a bug to
  chase. Models that rabbit-hole on it are over-fitting.
- **Device-free decomposition** (grade the parts models get wrong, no board):

  | Slice | Tier | Grader check |
  |-------|------|--------------|
  | Components added, builds for esp32s3 | T1 | `idf.py build` exits 0 |
  | Variant decision: `st77916_variant_from_id()` maps `{00,02,7f,7f}`→NEW, others→DEFAULT | T2 | `ctest` → `test_st77916_variant` green |
  | Off-glass touches dropped: `touch_in_circle()` | T2 | `ctest` → `test_touch_geom` green |
  | Call order: expander reset → `reset_gpio_num=-1` → selected init array → GRAM clear → LVGL | T3 | `ctest` → `test_display_sequence` green |
  | Crisp `hello`, no stripes | T5 | human / device |

  Slices 1–3 are live with zero hardware: the variant core is a pure function
  (`main/st77916_variant.c`) with a host test, and `test_display_sequence`
  compiles the real `display.c` against the `tests/host/fakes/` IDF mocks and
  asserts both the variant selection and the bring-up call order. Only the
  visible-pixels row needs a board.

## Phase 1c — audio bring-up

### AU-1 — Make the speaker play (tone, then WAV)
- **Anchor:** the "add esp_codec_dev" commit.
- **Prompt:** "Play sound out of the speaker — a test tone, then an embedded WAV."
- **Done-correctly:** audible tone (then WAV); log `audio: PCM5101 ready` +
  `audio: playing`. The killer footgun:
  1. **WRONG CODEC IN THE DEMO (the big one).** Waveshare's official ESP-IDF
     demo configures **ES8311 + ES7210 over I²C** — but those chips **are not on
     this board**. An I²C scan finds nothing at 0x18/0x40 (only touch 0x15,
     expander 0x20, RTC 0x51). The board's real audio, per the **schematic**, is
     a **PCM5101A I²S DAC** (→ NS8002 amp) for playback and an **ICS-43434 I²S
     MEMS mic** for capture — **both pure I²S, no I²C control bus.** A model that
     trusts the demo burns hours hunting a nonexistent codec (expander power?
     MCLK? reset line?) — all dead ends. The fix is *less* code: a plain I²S TX
     channel; no esp_codec_dev. Lesson: on the C variant, trust the schematic
     over the demo's `bsp_board.c`.
  2. **Continuous-tone underrun.** With I²S `auto_clear=false` (default), once a
     finite tone's samples are consumed the DMA repeats the stale buffer → the
     beep drones forever. Set `auto_clear=true` so underruns emit silence.
- **Device-free slices:** build (T1); sine-buffer fill `audio_fill_sine()` host
  test (T2). The playback path has no I²C/codec call-sequence to mock, so T3
  adds little here; "I hear it" needs the board (T5).
- **STATUS (1c-a done, 2026-05-31):** speaker plays a tone AND an embedded WAV
  from flash (rising chime). `audio.c` = master I²S TX to the PCM5101A; tone via
  host-tested `audio_tone`, WAV via host-tested `wav.c` parser + `EMBED_FILES`
  (`assets/boot.wav`), clock retuned to the WAV's rate. Remaining: **1c-b** —
  ICS-43434 I²S mic capture (SCK=15, WS=2, SD=39) → dump over serial → host WAV.

### AU-2 — Capture the mic to a WAV (done, 2026-05-31)
- **Anchor:** the WAV-playback commit.
- **Prompt:** "Record a few seconds from the mic and get it onto the laptop as a
  playable WAV."
- **Done-correctly:** `mic.c` brings up an I²S RX channel on the ICS-43434
  (I2S_NUM_1, SCK=15/WS=2/SD=39, no I²C), records to PSRAM, converts 24-bit→16
  (`mic_sample.c`, host-tested + clamped), and dumps base64 framed over serial;
  `tools/capture_mic.py` resets the board, reads the block (skipping interleaved
  log lines), and writes a WAV. Footguns: the mic is a *separate* I²S bus from
  the speaker DAC (use I2S_NUM_1); it's 24-bit-in-32-bit (read 32-bit slots);
  raw binary over the console is fragile, so base64 + markers + line filtering.
- **Device-free slices:** build (T1); the sample conversion `mic_sample_to_pcm16`
  host test (T2). Only "the recording sounds right" needs the board (T5).

## Phase 2 — WiFi + device↔bridge link

### NET-1 — Get the device talking to the bridge (the firewall red herring)
- **Anchor:** the "firmware WebSocket client" commit.
- **Prompt:** "The device joins WiFi but can't complete the WebSocket handshake
  with the bridge — the bridge logs 400, the device logs `Error read response
  for Upgrade header`. Fix it."
- **Done-correctly:** device logs `ws: <- {welcome...}`; bridge logs the hello +
  `sent welcome`. The trap: it looks like a protocol/code bug, but the request
  is valid (capture it with `nc` and replay it → the server returns 101). The
  real cause is the **macOS Application Firewall** on the host blocking the
  bridge's (venv/python.org) Python from receiving LAN connections: the TCP
  handshake completes and the kernel ACKs the request, but the process gets
  `ENOTCONN` and 400s. Loopback works (bypasses the firewall); `nc` works (an
  allowed binary). Fix = allow that Python in the firewall, not touch the code.
  A model that keeps editing the handshake will never succeed.
- **Device-free slices:** bridge `test_protocol.py` (encode/parse) + `test_server.py`
  (real handshake over loopback) — both T-equivalent of host tests, no device,
  and they *pass even when the device link is firewall-blocked*, which is the
  whole point: the bug is environmental, not in the code.

### NET-2 — Mic recording plays at half speed (I2S mono-RX footgun)
- **Anchor:** the "stream mic to bridge" commit (before the slot fix).
- **Prompt:** "The streamed mic recording plays back at half speed / too low."
- **Done-correctly:** recording plays at normal pitch. Footgun: on ESP32, I2S
  **RX in `I2S_SLOT_MODE_MONO` still delivers BOTH slots interleaved** (the mono
  setting mostly affects TX), so you capture 2× the samples → a 16 kHz-labelled
  WAV runs half-speed. Fix: configure STEREO and take only the mic's slot
  (here the *right*, index `i*2+1`; the left was silent). Tell: time the stream —
  3 s of audio finishing in ~1.5 s of wall-clock is the giveaway.
- **Device-free slice:** none for the timing itself (it's live I2S), but the
  2×-sample symptom is obvious from the bridge WAV duration vs stream wall-time.

### NET-3 — Bidirectional audio over the link (echo loopback)
- **Anchor:** the "Phase 2c-b" commit.
- **Prompt:** "Make the device play audio the bridge sends it, and have the bridge
  echo a received mic recording back so it comes out the speaker."
- **Done-correctly:** speak -> device streams mic -> bridge echoes -> device plays
  it. Pieces: device handles incoming WS **binary** frames (op 0x02 + 0x00
  continuation) -> `audio_play_pcm` to the DAC; bridge `send_wav` frames a WAV as
  `play_begin` -> binary PCM -> `play_end`. Footgun: playing in the WS event
  handler blocks the socket task during playback (fine for a short clip; a real
  build wants a ring buffer + playback task). The wire stays 16 kHz mono PCM, so
  no rate negotiation.
- **Device-free slices:** bridge `test_server.py` echo test (begin/binary/end
  round-trip over loopback) + `test_protocol.py` play-message builders (T-host).

### STT-1 — Bridge transcribes received audio (faster-whisper)
- **Anchor:** commit eb78e69 ("Phase 3a").
- **Prompt:** "When the device finishes streaming a mic recording, have the bridge
  transcribe it and log what was said."
- **Done-correctly:** after `audio_end`, the bridge logs `you said: "<text>"`.
  Pieces: a swappable `STT` interface (`stt.py`) with `WhisperSTT` (faster-whisper,
  lazy heavy import so tests/imports don't pull it) + a `FakeSTT`; the server runs
  STT off the event loop via `asyncio.to_thread` (whisper is blocking and slow —
  calling it inline stalls the socket / other connections). Tell that it's wrong:
  transcription inline blocks `async for message in ws` so a second device can't
  handshake while one is transcribing.
- **Device-free slice (T3-host):** `test_transcribes_on_audio_end` injects
  `FakeSTT` via `functools.partial(handle_connection, stt=fake)` and asserts the
  saved WAV was handed to STT exactly once — no model, no download, no board.

### TI-1 — "Reset loop" that's really the debugger resetting the board
- **Anchor:** any firmware with a boot banner; Phase 3a era.
- **Prompt:** "The board looks stuck in a reset loop — it boots, prints the
  banner, and reboots over and over whenever I watch the serial log. Find why."
- **Done-correctly:** realize the *observer* is the cause — opening the port with
  default pyserial (or many monitors) asserts DTR/RTS, which drives the ESP32
  auto-reset circuit, so every capture reboots the chip. Fix: open with line
  control disabled (`dtr=False, rts=False`, `dsrdtr=False`) to observe without
  resetting. Tell: the boot timestamp restarts at ~0 ms every time you attach;
  uptime never exceeds the time between captures.
- **Device-free slice:** none (it's a host-tooling/electrical behavior), but it's
  a strong *debugging-reasoning* eval — the bug is in how you look, not the code.

### FW-1 — Touch driver floods the console on idle (CST816 standby NACK)
- **Anchor:** Phase 1b touch bring-up onward (latent; surfaces when idle).
- **Prompt:** "The console is flooded with `CST816S: I2C read failed` /
  `i2c transaction failed` when nobody's touching the screen. Stop the spam
  without breaking touch."
- **Done-correctly:** the CST816 NACKs I2C reads while idle/in standby; `touch_task`
  polling it logs an error every cycle (~20/s × 4 lines). Fix options: gate reads
  on the touch INT line, treat a NACK as "no touch" silently, or rate-limit the
  error log — touch must still report real presses. Tell: a healthy fix produces
  *zero* error lines at idle yet still logs/handles a real touch.
- **Device-free slice:** extract the "interpret a read result/NACK into a touch
  event-or-nothing" decision into a pure function and host-test it (T-host); the
  silence-at-idle behavior itself needs the board (T5).

### TTS-2 — Bridge speaks a reply back (Piper talk-back)
- **Anchor:** commit 27c25b2 ("Phase 3b").
- **Prompt:** "After the bridge transcribes the mic recording, have it speak a
  reply back out the device's speaker."
- **Done-correctly:** speak -> device plays back "You said: <transcript>". Pieces:
  a swappable `TTS` interface (`tts.py`) with `PiperTTS` (lazy import) + `FakeTTS`,
  synthesis run off the event loop (`asyncio.to_thread`), reusing `send_wav()`.
  **Footgun:** the default Piper voice MUST be a **16 kHz** ("low") voice — the
  device's binary-PCM playback plays at a fixed 16 kHz clock and ignores
  `play_begin`'s rate, so a 22 kHz "medium" voice plays ~38% fast and high-pitched.
  Tell it's wrong: the reply is intelligible but chipmunk-fast.
- **Device-free slice (T3-host):** `test_speaks_reply_on_audio_end` injects
  `FakeSTT`+`FakeTTS`, asserts the reply text is `"You said: <x>"` and that a
  `play_begin` -> binary -> `play_end` stream goes back over loopback — no model.

### PWR-1 — Cold-boot "flash/hello" loop that settles (brownout, diagnosed by absence)
- **Anchor:** any WiFi+display+audio firmware on the C board.
- **Prompt:** "On a cold USB plug the board flashes and restarts in a loop for a
  few seconds, then settles and runs fine. Find the cause."
- **Done-correctly:** identify it as **brownout** — backlight + WiFi RF calibration
  (+ boot chime) draw too much current in the first ~second, sag the 3.3 V rail,
  reset before USB enumerates, repeat until things stabilize. Confirm via a
  stronger supply (loop vanishes) and/or `esp_reset_reason()`. The *reasoning* is
  the eval: on USB-Serial-JTAG the port is the MCU's own USB, enumerating ~1–3 s
  into boot, so a reconnecting serial capture grabs **zero** ROM banners/backtraces
  during the loop — that absence rules out a logic abort (which runs past USB-up
  and leaves a backtrace) and points at early-power. Fixes: stronger supply;
  firmware-side stagger boot current (defer chime, ramp backlight post-assoc).
- **Verification tier:** T5 (needs the board + a weak vs. strong supply); the
  reasoning step is gradeable from a transcript (does the model reason from the
  missing banners rather than demand a backtrace that can't exist?).

### VAD-1 — Server-side endpointing of a continuous mic stream
- **Anchor:** commit b568cf2 ("Phase 3c-a").
- **Prompt:** "The device now streams the mic continuously. Have the bridge split
  that stream into utterances on silence and transcribe each one."
- **Done-correctly:** an energy/RMS `Endpointer` (pre-roll for word onsets,
  silence hangover so short pauses don't split a phrase, a max-utterance cap)
  emits utterance PCM blobs the server transcribes+answers; audio_end flushes the
  in-progress one. Tell it works: full natural sentences segment as variable-length
  utterances (not fixed clips); pure silence yields nothing.
- **Device-free slice (T2/T3):** `test_vad.py` drives the Endpointer with
  synthetic silence/tone arrays; `test_server.py` feeds voiced vs. silent PCM over
  loopback and asserts WAV/STT/segmentation behavior — no model, no board.

### HD-1 — Half-duplex echo: mute the mic for the reply's real duration
- **Anchor:** after 9d10e62 (the bug is live there).
- **Prompt:** "When the bridge speaks a reply, the device sometimes transcribes
  its own output back (reply 'You said: Hello' returns as 'Did say hello'). Stop
  the echo without cutting off the user."
- **Done-correctly:** realize the mute releases too early — the device unmutes a
  fixed tail after `play_end` is *received*, but play_end arrives when audio is
  queued, not when the speaker finishes, so the clip's tail leaks into the mic.
  Fix: the bridge knows the clip length (PCM bytes ÷ (rate·bytes/sample)); send it
  in play_begin and mute for duration+tail (or otherwise gate on playback-complete).
  Tell it's wrong: only SHORT replies echo (long ones finish before the tail
  expires), and the echo text rhymes with the spoken reply.
- **Device-free slice:** the gate decision is already pure (`mic_gate`,
  test_mic_gate.c, tier T2) — extend it to take a duration; the acoustic echo
  itself needs the board (T5).

<!-- Append new ideas below as phases land. -->

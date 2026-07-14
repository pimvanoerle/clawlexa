# clawlexa — Design Spec

> Living document. Update as decisions land. Each section marks **Decided** vs
> **Open** so we can tell at a glance what's still fluid.

## 1. Vision

Give Claude-powered agents a body: a small always-on device that listens for a
wake word, speaks back, and exposes a touch screen for ambient interaction.

clawlexa is **not** an agent itself. It is a peripheral that any agent can
plug into via MCP. MVP target: [iPinch](#mvp-target-ipinch). Designed so
ourclaw, spark, Claude Desktop, or future agents can drop in with no device-side
changes.

## 2. MVP target: iPinch

iPinch is the user's homebrew Claude Code wrapper:
- Runs on a laptop
- Slack is the primary chat surface
- Uses MCP servers for its tooling (architecturally similar to a lighter
  "openclaw")

clawlexa joins iPinch as another MCP server. When the user wakes the device and
speaks, iPinch receives the transcript as if it were a Slack message (or
through a dedicated voice channel — TBD); when iPinch wants to reply audibly,
it calls a `speak` tool.

## 3. Hardware

**Board:** Waveshare ESP32-S3-Touch-LCD-1.85C ([product page][waveshare])

- ESP32-S3R8 (dual-core Xtensa LX7, 8 MB PSRAM, 16 MB flash)
- 1.85" 360×360 round IPS LCD, capacitive touch
- Onboard MEMS microphone
- Onboard speaker driver + small speaker
- WiFi 2.4 GHz + Bluetooth 5 LE
- USB-C, LiPo battery support, RTC
- QMI8658 IMU (orientation / tap detection — bonus)

[waveshare]: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C

Future hardware variants (different enclosures, larger speakers, multiple
mics) should be supported by keeping the firmware's audio + display layer
behind clean interfaces.

## 4. System architecture

```
┌──────────────┐   WiFi      ┌──────────────────┐   MCP    ┌────────────────┐
│  ESP32-S3    │ ─ WebSocket ─▶  Host bridge    │ ─ stdio ─▶  Agent (iPinch │
│  (firmware)  │ ◀── audio ── │  (Python/Rust)  │ ◀── tool ─│   / ourclaw …) │
└──────────────┘             └──────────────────┘  results  └────────────────┘
```

**Decided:**
- Bridge lives on the **same host** as the agent. iPinch runs on a laptop;
  clawlexa-bridge runs there too. No cloud component in v1.
- Bridge speaks MCP to the agent. Device speaks a private bidirectional
  protocol to the bridge.
- Device is responsible for wake-word detection so we don't stream audio
  continuously across the LAN.
- **Transport: WebSocket** (binary frames over TCP). Built-in framing,
  NAT-friendly, easy to debug; `esp_websocket_client` on firmware, `websockets`
  on the bridge. Control = JSON text frames; audio = raw binary frames.
- **Wire audio codec: 16 kHz mono 16-bit PCM** for v1 (no device encoder,
  matches the mic/speaker pipeline, ~256 kbps is fine on a LAN). Opus later.
- **Discovery: hardcoded** bridge host:port + WiFi creds via Kconfig for v1
  (movable to NVS). mDNS auto-discovery and BT/SoftAP provisioning come later.
- **Auth: trust the LAN** for v1. A shared secret in NVS comes later.

**Open:**
- When to graduate provisioning from Kconfig → NVS → mDNS/BT, and add the
  shared-secret handshake (all deferred past the v1 link bring-up).

## 5. MCP server surface

The bridge exposes a small set of MCP tools and notifications. Naming TBD;
sketch:

**Tools (agent → device):**
- `speak(text, voice?)` — synthesize and play on device
- `show(content)` — push a status / message / image to the LCD
- `set_state(idle|listening|thinking|speaking|error)` — drives the display's
  ambient indicator
- `listen(timeout_ms?)` — open a listening window without requiring wake word
  (push-to-talk style)
- `stop_speaking()` — interrupt current TTS playback

**Notifications (device → agent, via MCP server-initiated messages or a
polling tool):**
- `utterance_available(text, confidence, audio_ref?)` — wake word fired,
  user spoke, transcript ready
- `touch(event, x?, y?)` — tap / long-press / swipe
- `wake_detected()` — fired the moment the wake word triggers, before STT
  completes (so the agent can pre-empt)
- `device_status(battery, rssi, ...)` — periodic health

**Implemented (Phase 5, v1):**
- **Transport: stdio** (FastMCP) — how an agent normally adds an MCP server.
  `python -m clawlexa_bridge --mcp` runs the device WebSocket server + the MCP
  stdio server on one loop, sharing a `Hub` (utterance queue + speak()).
- **Tools: `wait_for_utterance(timeout_ms?)`** (blocks until the user speaks
  after the wake word; returns the transcript) and **`speak(text)`**. Pull-based
  `wait_for_utterance` resolves the server-push question below — the agent pulls
  rather than the server pushing. In MCP mode the device's transcript goes to the
  agent (no standalone echo); the agent replies via `speak`.
- Deferred to later phases: `show`/`set_state` (need display states — Phase 6),
  `listen`/`stop_speaking`, and the `touch`/`wake_detected`/`device_status`
  events. HTTP/SSE transport (for a persistent multi-agent bridge) is a later option.

**Open:**
- MCP doesn't have a great story for *server-pushed* notifications to all
  clients yet — sidestepped for v1 via the pull-based `wait_for_utterance`.
- Whether `utterance_available` should fire automatically or whether the agent
  must explicitly `arm()` after a wake event.

## 6. Voice pipeline

```
mic ─▶ ring buffer ─▶ wake-word model ─▶ (on trigger) ─▶ stream to bridge
                                                        │
                                                        ▼
                                              bridge: VAD ─▶ STT ─▶ MCP event
                                                                       │
                                                        ┌──────────────┘
                                                        ▼
                                              agent reply text
                                                        │
                                              bridge: TTS ─▶ stream to device
                                                        │
                                                        ▼
                                              device: I²S out ─▶ speaker
```

**Decided:**
- Wake word runs on-device, audio only leaves the device after it fires.
- STT and TTS run on the bridge (CPU/GPU is free there; keeps firmware lean).
- Half-duplex for v1 (no barge-in while clawlexa is speaking). Full-duplex
  with echo cancellation is a stretch goal. A wake opens a **multi-turn
  conversation window** (see §7): the user can take several turns without
  repeating the wake word; the bridge ends the conversation on silence.
- Device audio I/O is **16 kHz / 16-bit mono PCM** over I²S. The 1.85C has **no
  I²C codec** (the Waveshare demo's ES8311/ES7210 config is KORVO-derived and
  does not match this board): playback is a **PCM5101A** I²S DAC → NS8002 amp;
  capture is an **ICS-43434** I²S MEMS mic on a separate I²S bus. No codec
  component needed — plain I²S TX/RX. See [hardware/PINOUT.md](./hardware/PINOUT.md).
- **STT: local `faster-whisper`** on the bridge (offline, no API keys, private;
  fast on Apple Silicon). **TTS: local `Piper`** (offline, no keys). Both sit
  behind a small interface so a cloud engine can drop in later; engine choice is
  configurable. v1 is all-local.
- **Endpointing: server-side VAD** on the bridge (energy-based for v1) — the
  device streams; the bridge segments utterances on silence.

**Open:**
- Whether to add cloud STT/TTS options (better TTS voices) behind the interface,
  and a config knob to pick the engine — deferred until the local path works.

## 7. Wake word

Goal: detect the device's name continuously without streaming audio, with low
enough CPU/power that it can run alongside the rest of the firmware.

Candidates:

| Option            | Pro                                                   | Con                                          |
|-------------------|-------------------------------------------------------|----------------------------------------------|
| **ESP-Skainet**   | First-party Espressif, ESP32-S3 optimized (HiFi MAC) | Custom wake words via Espressif's portal (turnaround ~weeks) |
| **microWakeWord** | Open source, train your own via Colab, used by ESPHome | Slightly higher latency, fewer pre-built models |
| **Porcupine**     | Excellent accuracy, easy training UI                 | Commercial license required beyond personal use |

**Decided (Phase 4):**
- **Engine: microWakeWord.** Open source (Apache-2.0), runs on the ESP32-S3 via
  TFLite-Micro, custom words trained yourself in a free Colab notebook (no
  account). It's what ESPHome / Home Assistant voice satellites use. *We
  originally tried Porcupine (Picovoice) for its instant custom keyword, but
  reversed it: Picovoice requires a B2B "valid company email" signup and a
  commercial license — incompatible with an open-source project.* ESP-SR/WakeNet
  was the other option but can't do a custom word without Espressif's slow portal.
- **Wake word: `clawlexa`** for the generic build (3 syllables, detection-
  friendly, on-brand). The model is a **committed `.tflite` asset** — it's ours,
  Apache-licensed, so we check it into the repo and cloners get a working wake
  word with no setup. A different word (e.g. iPinch → `hey pinchy`) is a **model
  swap**: retrain via the notebook, drop in the new `.tflite`, point a config knob
  at it. No per-clone manual step for the default word.
- **Bring-up plan.** De-risk the firmware integration first with a *pre-trained*
  community model (e.g. "okay nabu" / "hey jarvis"), prove the on-device pipeline
  end-to-end, then swap in the trained `clawlexa` model. The custom word is then
  just a file, not a blocker.
- **Gating architecture.** The wake detector flips a small device-side state
  machine LISTENING→STREAMING; the mic only streams after the word fires,
  and returns to LISTENING when the conversation ends. The transition logic is a
  pure, host-tested core (`wake_gate`); the detector and the IO (start/stop
  streaming) are the swappable edges.
- **Multi-turn conversation window (Phase 6b).** A wake opens a *conversation*,
  not a single turn: after the agent's reply plays, the mic keeps streaming for a
  follow-up window (~12 s) so the user can continue without repeating the
  wake word; silence past the window re-arms the wake detector (a spoken farewell
  ends it immediately — see below). **Conversation
  end is bridge-driven**, not device-driven — the bridge owns the VAD and sees
  the agent's activity, so it decides when the conversation is idle and sends an
  `end_turn` control frame; the device then stops streaming and returns to
  wake-gated LISTENING. This keeps the firmware dumb (no on-device silence
  timing) and makes the window logic Python/Layer-3-testable. The window
  **pauses while the agent is thinking or speaking** — a pending reply never
  times out into a re-arm (this also retires the old device-side 10 s no-reply
  timeout, which was too short for a cold-start first turn); a longer safety
  timeout covers an agent that never replies. The pure `wake_gate`
  (LISTENING↔STREAMING) is unchanged; only the trigger for TURN_END moves from
  device-side `play_end` to the bridge's `end_turn`. Utterances spoken while the
  agent is thinking already queue on the bridge (`Hub._utterances`) and drain in
  order — "fire off two thoughts" works without a re-wake. Barge-in (talking over
  the reply) still needs AEC — deferred to Phase 8.
- **Natural conversation end (goodbye).** Rather than always waiting out the
  silence window, the conversation ends the moment it's clearly over: the agent
  appends a `<end>` sentinel to its reply when it judges the chat done (stripped
  before TTS), and the host also matches farewell phrases in the user's transcript
  as a backstop. Either calls the `end_conversation` MCP tool →
  `Conversation.end_now()` → the watchdog sends `end_turn` immediately, so after a
  "bye" the device re-arms right after the farewell plays instead of sitting
  attentive for the full window.

## 8. Display & touch

Phase 1 ambition is minimal: a single status indicator (color + small icon +
optional caption) reflecting `idle | listening | thinking | speaking | error`.

Later: agent-pushed content (a generated image, a Slack message preview, a
calendar reminder), tap-to-confirm flows, swipe to dismiss.

**Decided:**
- Display state is driven by the agent via `set_state` / `show`, not inferred
  on-device. Keeps the firmware dumb.
- **UI framework: LVGL**, via the `esp_lvgl_port` managed component (LVGL 9).
  Hand-rolling anti-aliased arcs/text on a round panel isn't worth it, and the
  eventual status indicator (§8 "Later") wants a real widget toolkit. Panel
  driver is `esp_lcd_st77916` (QSPI); touch is `esp_lcd_touch_cst816s`.
- **Panel reset is gated by a PCA9554 IO expander** (not a direct GPIO) — the
  init path must pulse LCD reset through the expander before talking to the
  ST77916, and pass `reset_gpio_num = -1` to the panel driver. See
  [hardware/PINOUT.md](./hardware/PINOUT.md) for the confirmed C-variant pin
  map.

**Open:**
- How to represent "agent is thinking" visually on a round screen — a
  rotating arc seems obvious.

## 9. Firmware stack

**Decided:**
- **ESP-IDF + `idf.py`** (no PlatformIO wrapper). Espressif's first-party
  stack, used by their own examples and by `pytest-embedded`. Audio
  peripherals, LVGL port, and the wake-word libraries (ESP-Skainet,
  microWakeWord) all target IDF directly.
- Target **ESP-IDF v5.4+** (board needs S3R8 / QSPI display support).
- **Managed components** (via `idf_component.yml`) for everything we can —
  display driver, touch driver, LVGL port, audio codec — rather than vendoring
  Waveshare's demo wholesale. Vendored code rots.

**Open:**
- Whether to ever vendor pieces of the Waveshare factory demo for things the
  component registry doesn't cover yet.

## 10. Bridge stack

**Decided:**
- **Python** for v1 — `mcp` SDK exists, `websockets` for the device link, easy
  STT/TTS libs later, fast iteration. Lives in `bridge/`.
- Ships as a **one-shot runnable script/module** for v1 (`python -m clawlexa_bridge`
  or similar); a `pipx`-installable CLI can come later.

**Open:**
- Packaging polish (pipx/entry-point) and config file format — deferred until
  the bridge does more than echo the link.

## 10a. Testing strategy

Three layers, with explicit boundaries so each has a job to do and we don't
end up with one slow flaky suite.

**Layer 1 — host unit tests (firmware logic).** Pure C modules with no
ESP-IDF / hardware dependencies. Compiled with the host toolchain and Unity,
run as a normal binary. Fast (<1s), runs in CI on every push, no board
required. Lives in `firmware/tests/host/`.

**Layer 2 — on-device integration tests.** `pytest-embedded` drives a real
ESP32-S3 over USB-serial, asserts on log output and serial responses. Slow
(seconds), requires the board, run by humans pre-merge for now. Lives in
`firmware/tests/pytest/`. Self-hosted HiL CI is a stretch goal.

**Layer 3 — bridge unit tests.** Standard `pytest` for the host bridge, with
fakes for the device-side protocol. Lives in `bridge/tests/` (Phase 2+).

**Rules of the road:**
- Anything that can be tested at Layer 1 *must* be — i.e. protocol parsers,
  state machines, ring buffers, codec framing. Don't put non-IO logic inside
  IO modules.
- Layer 2 tests assert *observable* behavior (boot logs, serial responses),
  not internals.
- Every PR has at least one test added or modified, unless it's purely
  documentation. CI enforces no regressions; reviewers enforce the spirit.

## 11. Repo layout (intended)

```
clawlexa/
├── firmware/        # ESP-IDF project for the device
├── bridge/          # Host-side MCP server + device protocol
├── wakeword/        # Training notebooks + model artifacts
├── hardware/        # Pinouts, enclosure notes, BOM
├── docs/            # Diagrams, screenshots, decisions
├── SPEC.md
├── CLAUDE.md
└── README.md
```

Nothing in here yet — created as each phase starts.

## 12. Roadmap

- [x] **Phase 0** — repo + spec
- [x] **Phase 1** — Hardware bring-up: display "hello" (LVGL/ST77916) + touch
      (CST816); play a WAV from flash through the speaker (PCM5101A DAC); capture
      mic (ICS-43434) to a WAV over USB serial via `firmware/tools/capture_mic.py`.
- [x] **Phase 2** — WiFi (station) + device↔bridge WebSocket transport. Mic
      streams to the bridge (saved as WAV); bridge streams PCM back to the
      speaker (verified via a mic→bridge→speaker echo loopback).
- [x] **Phase 3** — Bridge does STT and TTS round-trip (no agent yet — just a
      loop-back: "you said X"). **3a (STT)** faster-whisper; **3b (TTS talk-back)**
      Piper, both verified live (speak → device → STT → "You said: X" → device
      speaks it). **3c**: device streams the mic continuously (a FreeRTOS task,
      no boot-time timing race); the bridge endpoints utterances with energy VAD
      (`vad.py`); half-duplex — the device mutes its mic across the bridge's
      playback (`mic_gate`, host-tested). 3a/3b's racy once-per-boot trigger is
      retired.
- [~] **Phase 4** — Wake word on device. Audio only streams post-trigger.
      **Firmware DONE + verified** (microWakeWord via esp-tflite-micro): device
      listens locally, and only after the wake word fires does it stream the
      command to the bridge, get a spoken reply, and return to silent listening
      (`wake_detector` + `wake_gate` gating `mic_stream_task`). Bring-up word is
      `okay nabu`; remaining: train + swap in a custom `clawlexa` model (reusable
      trainer so iPinch can mint `okay iPinch`), and write the README docs.
- [~] **Phase 5** — MCP server wrapper around the bridge. Wire to iPinch.
      **Bridge MCP server done + verified live** (`--mcp`, stdio): `wait_for_utterance`
      + `speak` over a shared `Hub`. A demo MCP agent (`bridge/tools/mcp_demo.py`)
      drove the device end-to-end — wake → command → agent picks a reply → spoken
      back. Build-time integration tests cover the protocol path. Remaining: wire
      the real iPinch (swap `demo_reply` for its LLM); minor: wake word can bleed
      into the transcript.
- [x] **Phase 6** — Display states + basic touch. **Done + verified on device:**
      `set_state`/`show` MCP tools → LVGL per-state color + an ASCII crab face per
      mood (sleeping/attentive/pondering/happy/x-eyes), agent-driven. Touch fixed
      (CST816 EXIO0/1 reset) and wired: **tap-to-talk** (a tap opens a turn like
      the wake word) + tap-to-cancel, debounced. Optional follow-ups: bitmap crab
      icons (ASCII is the no-asset fallback); a mute gesture; and a small tap
      dead-zone right after a reply — taps are dropped during the half-duplex
      mute tail (~300 ms past playback); fix is to queue the tap (don't clear the
      flag while muted) so it fires when the mute clears.
- [~] **Phase 6b** — Conversation flow: a wake opens a multi-turn window so
      follow-ups need no re-wake. Conversation end is **bridge-driven** (the
      bridge sends `end_turn` after ~12 s of real silence, or immediately on a
      goodbye — `<end>` sentinel from the agent or a farewell phrase from the
      user; the window pauses while the agent is thinking/speaking). Think-time
      utterances already queue on the bridge. `wake_gate` is unchanged — only the
      TURN_END trigger moves from device `play_end` to the bridge `end_turn`.
      Barge-in deferred to Phase 8.
- [ ] **Phase 7** — Second-agent integration (ourclaw or Claude Desktop) to
      prove the MCP boundary is real.
- [ ] **Phase 8+** — Stretch: barge-in, on-screen content from agent, IMU
      gestures, battery-life tuning, custom enclosure.

## 13. Non-goals (v1)

- Multi-room / multi-device fleets.
- Speaker identification or voice biometrics.
- Doing the agent loop on-device — clawlexa is always a peripheral.
- Privacy-preserving STT/TTS by default — we'll document the trade-off and
  let users pick local models, but cloud is fine for MVP.

## 14. Open decisions, consolidated

A single list to make easy to triage; each links to its section above.

- [x] ~~Device↔bridge transport~~ → WebSocket (binary frames) (§4)
- [x] ~~Audio codec on wire~~ → 16 kHz mono PCM for v1; Opus later (§4)
- [x] ~~Device discovery / pairing~~ → hardcoded via Kconfig for v1; mDNS later (§4)
- [ ] MCP push-notifications mechanism (§5)
- [x] ~~STT engine default~~ → local `faster-whisper` (cloud later) (§6)
- [x] ~~TTS engine default~~ → local `Piper` (cloud later) (§6)
- [x] ~~Endpointing~~ → server-side VAD on the bridge (§6)
- [ ] Wake-word engine: microWakeWord vs ESP-Skainet (§7)
- [ ] Actual wake word phrase (§7)
- [x] ~~On-device UI framework~~ → LVGL via `esp_lvgl_port` (§8)
- [x] ~~Firmware framework~~ → ESP-IDF + `idf.py`, v5.4+ (§9)
- [x] ~~Bridge language~~ → Python, shipped as a one-shot script for v1 (§10)

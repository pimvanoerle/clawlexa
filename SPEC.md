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

**Open:**
- Device ↔ bridge transport: WebSocket (binary frames) vs raw TCP vs UDP+RTP
  vs MQTT. Leaning WebSocket — easy to debug, framing built-in, works through
  most network setups.
- Audio codec on the wire: 16 kHz PCM (simple, ~256 kbps) vs Opus (40–64 kbps,
  needs encoder on ESP32 — Espressif has an Opus port).
- Pairing / discovery: hardcoded host IP in NVS vs mDNS auto-discovery vs
  Bluetooth provisioning.
- Auth between device and bridge: shared secret in NVS, or just trust LAN.

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

**Open:**
- MCP doesn't have a great story for *server-pushed* notifications to all
  clients yet — confirm whether to use MCP `notifications/*` channels or a
  separate subscription tool.
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
  with echo cancellation is a stretch goal.

**Open:**
- STT engine: local `whisper.cpp` (offline, free, ~1s latency on M-series)
  vs cloud (Deepgram, OpenAI Realtime, Groq Whisper). Likely *configurable*
  with a sensible default.
- TTS engine: cloud (ElevenLabs, OpenAI, Cartesia) vs local (Piper, Kokoro).
  Same — configurable, default to whichever sounds best for the MVP demo.
- Endpointing: server-side VAD vs let STT model decide.

## 7. Wake word

Goal: detect the device's name continuously without streaming audio, with low
enough CPU/power that it can run alongside the rest of the firmware.

Candidates:

| Option            | Pro                                                   | Con                                          |
|-------------------|-------------------------------------------------------|----------------------------------------------|
| **ESP-Skainet**   | First-party Espressif, ESP32-S3 optimized (HiFi MAC) | Custom wake words via Espressif's portal (turnaround ~weeks) |
| **microWakeWord** | Open source, train your own via Colab, used by ESPHome | Slightly higher latency, fewer pre-built models |
| **Porcupine**     | Excellent accuracy, easy training UI                 | Commercial license required beyond personal use |

**Leaning:** start with **microWakeWord** — open, trainable in a notebook,
already runs on ESP32-S3 in the wild (ESPHome voice assistant satellites use
it). Fall back to ESP-Skainet if accuracy isn't good enough.

**Open:**
- The actual wake word. Working name: "Hey Claw" or just "Claw". Needs to be
  >= 3 syllables for reliable detection; "Claw" alone is borderline.
- Whether to ship multiple wake words (one per agent — "Hey Pinch", "Hey
  Spark") or stay with one neutral word.

## 8. Display & touch

Phase 1 ambition is minimal: a single status indicator (color + small icon +
optional caption) reflecting `idle | listening | thinking | speaking | error`.

Later: agent-pushed content (a generated image, a Slack message preview, a
calendar reminder), tap-to-confirm flows, swipe to dismiss.

**Decided:**
- Display state is driven by the agent via `set_state` / `show`, not inferred
  on-device. Keeps the firmware dumb.

**Open:**
- UI framework on-device: LVGL (heavy, polished) vs raw framebuffer drawing
  (lean, ugly). Waveshare ships LVGL examples — probably just use that.
- How to represent "agent is thinking" visually on a round screen — a
  rotating arc seems obvious.

## 9. Firmware stack

**Open:**
- ESP-IDF (C, full control, first-class Espressif support) vs Arduino-ESP32
  (faster bring-up, less mature for audio + LVGL combo on this board) vs
  **PlatformIO + ESP-IDF** (good middle ground).
- Whether to start from the Waveshare factory demo and strip it down, or
  build up from a blank ESP-IDF project.

Leaning: **PlatformIO + ESP-IDF**, starting from the Waveshare LVGL demo so
display + touch work day one, then bolt on audio and networking.

## 10. Bridge stack

**Open:**
- Language: Python (fast iteration, `mcp` SDK exists, easy STT/TTS libs) vs
  Rust/Go (lower footprint, no dep hell). Likely **Python** for v1 — the
  laptop has the cycles, and we want to iterate fast.
- Whether to ship as a one-shot script or as a `pipx`-installable CLI
  (`clawlexa-bridge`).

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
- [ ] **Phase 1** — Hardware bring-up: display "hello", capture mic to local
      file over USB serial, play a WAV from flash through the speaker.
- [ ] **Phase 2** — WiFi bring-up + device↔bridge transport. Stream mic audio
      to bridge, stream TTS bytes back.
- [ ] **Phase 3** — Bridge does STT and TTS round-trip (no agent yet — just a
      loop-back: "you said X").
- [ ] **Phase 4** — Wake word on device. Audio only streams post-trigger.
- [ ] **Phase 5** — MCP server wrapper around the bridge. Wire to iPinch.
- [ ] **Phase 6** — Display states + basic touch (push-to-talk, mute,
      cancel).
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

- [ ] Device↔bridge transport (§4)
- [ ] Audio codec on wire (§4)
- [ ] Device discovery / pairing (§4)
- [ ] MCP push-notifications mechanism (§5)
- [ ] STT engine default (§6)
- [ ] TTS engine default (§6)
- [ ] Wake-word engine: microWakeWord vs ESP-Skainet (§7)
- [ ] Actual wake word phrase (§7)
- [ ] On-device UI framework (§8)
- [ ] Firmware framework: ESP-IDF vs Arduino vs PlatformIO (§9)
- [ ] Bridge language: Python vs Rust/Go (§10)

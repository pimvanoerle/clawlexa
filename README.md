# clawlexa

```
            (o) (o)
             |   |
             |   |             _
       ___   |   |   ___      (_)
      /   \  |   |  /   \      |
      \___/  |   |  \___/      T
        \\___|   |___//
        /             \
       |     \___/     |
        \_____________/
         |  |  |  |  |
         v  v  v  v  v
```

A voice, hearing, and touch interface for Claude-powered agents — built on the
Waveshare ESP32-S3-Touch-LCD-1.85C.

clawlexa runs as an MCP server on the same host as your agent, so anything that
speaks MCP (iPinch, ourclaw, spark, Claude Desktop, …) can give itself a face
and a voice by connecting to it.

Status: **Phase 4 — the voice loop works.** Say the wake word and the device
streams your command to the host bridge, which transcribes it (faster-whisper),
and speaks a reply back (Piper) — all local, then it returns to silent
listening. Hardware bring-up (display/touch/speaker/mic), WiFi transport, the
STT↔TTS round-trip, and the on-device wake word are done and verified on
hardware. Next: the MCP server wrapper (Phase 5) so any agent can plug in.

### Wake word — heads up if you're cloning this 🦀

The device only streams audio **after** an on-device wake word fires (so it
isn't listening to the room continuously). It ships with a working default word,
**"okay nabu"**, so it works out of the box — **no signup, no account, nothing to
download.** To use a **custom** word (e.g. `clawlexa`, or `okay iPinch` for an
iPinch build), you train a small [microWakeWord](https://github.com/OHF-Voice/micro-wake-word)
model and drop it in — see **[training/README.md](./training/README.md)** for the
full (phrase-parameterized) recipe and the one-block firmware swap.

See [SPEC.md](./SPEC.md) for the design, [firmware/README.md](./firmware/README.md)
for build/flash, and [CLAUDE.md](./CLAUDE.md) for working-with-Claude-Code notes.

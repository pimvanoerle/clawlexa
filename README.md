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

Status: **in daily use.** Say the wake word and the device streams your command
to the host bridge, which transcribes it (faster-whisper) locally, hands it to
your agent over MCP, and speaks the reply (Piper). One wake opens a multi-turn
conversation; the crab on the screen shows idle / listening / thinking /
speaking. Optional extras: a Home Assistant presence greeting (walk in, it says
hi and listens without a wake word) and a Claude voice driver that can use your
agent's MCP tools. See [SPEC.md §12](./SPEC.md) for what's done and what's next.

### Wake word — heads up if you're cloning this 🦀

The device only streams audio **after** an on-device wake word fires (so it
isn't listening to the room continuously). It ships with a working default word,
**"okay nabu"**, so it works out of the box — **no signup, no account, nothing to
download.** To use a **custom** word (e.g. `clawlexa`, or `okay iPinch` for an
iPinch build), you train a small [microWakeWord](https://github.com/OHF-Voice/micro-wake-word)
model and drop it in — see **[training/README.md](./training/README.md)** for the
full (phrase-parameterized) recipe and the one-block firmware swap.

**Connecting your own agent?** See **[docs/connect-your-agent.md](./docs/connect-your-agent.md)** —
get the bridge running on your laptop and wire your MCP agent (iPinch, Claude
Desktop, …) to the device's `wait_for_utterance` / `speak` tools.

See [SPEC.md](./SPEC.md) for the design, [firmware/README.md](./firmware/README.md)
for build/flash, and [CLAUDE.md](./CLAUDE.md) for working-with-Claude-Code notes.

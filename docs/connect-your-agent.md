# Connect your agent to clawlexa

This is the send-this-to-the-other-laptop guide: get the bridge running on the
machine where your agent lives (e.g. iPinch), and wire your agent to clawlexa
over **MCP**. The device talks to the bridge over WiFi; your agent talks to the
bridge over MCP. You don't need the firmware toolchain here — just Python and the
device already flashed and on your network.

```
clawlexa device ──WiFi/WebSocket──> bridge (this laptop) ──MCP (stdio)──> your agent
```

## 1. Prerequisites

- The **clawlexa device**, flashed and powered, on the **same LAN/WiFi** as this
  laptop. (Its firmware's `Bridge host` must point at this laptop's LAN IP — set
  via `idf.py menuconfig` → clawlexa → Bridge host, e.g. `192.168.1.221`.)
- **Python 3.10+** on this laptop.
- ~1 GB free for the local STT/TTS models (downloaded on first run).

## 2. Get the code + set up the bridge

```bash
git clone <clawlexa repo url> clawlexa
cd clawlexa/bridge
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt        # websockets, faster-whisper, piper-tts, mcp
```

First run downloads the faster-whisper model and the Piper voice (a few hundred
MB), then they're cached.

## 3. Network gotcha (macOS): allow the bridge through the firewall

If the device can't complete the WebSocket handshake (bridge logs a `400`, device
logs an Upgrade-header error) but the loopback tests pass, it's almost always the
**macOS Application Firewall** blocking the bridge's Python from receiving LAN
connections. Allow the **framework Python binary** (not `bin/python3`):

```
System Settings → Network → Firewall → Options →
  allow  /Library/Frameworks/Python.framework/Versions/<X.Y>/Resources/Python.app/Contents/MacOS/Python
```

then restart the bridge. (Loopback + `nc` work even when this is blocking, which
is what makes it sneaky.)

## 4. Run the bridge as an MCP server

```bash
.venv/bin/python -m clawlexa_bridge --mcp
```

This serves the device (WebSocket on `:8765`) **and** exposes the MCP tools over
stdio. Logs go to stderr; stdout is the MCP protocol stream. (Drop `--mcp` to run
the standalone "You said: X" echo loop for a quick smoke test without an agent.)

Normally you don't run this by hand — your agent spawns it (next step).

## 5. Wire your agent

clawlexa exposes these MCP tools:

| Tool | What it does |
|------|--------------|
| `wait_for_utterance(timeout_ms?)` | Blocks until the user speaks to the device (after the on-device wake word fires) and returns the transcript. Returns `""` on timeout. |
| `speak(text)` | Speaks `text` aloud on the device (TTS). |
| `set_state(state)` | Sets the device's ambient mood/indicator: `idle` \| `listening` \| `thinking` \| `speaking` \| `error`. |
| `show(text)` | Shows a short line of text on the device screen. |

Add the bridge as an MCP server in your agent's config (Claude Code / Claude
Desktop style — adapt for iPinch):

```json
{
  "mcpServers": {
    "clawlexa": {
      "command": "/abs/path/to/clawlexa/bridge/.venv/bin/python",
      "args": ["-m", "clawlexa_bridge", "--mcp"],
      "cwd": "/abs/path/to/clawlexa/bridge"
    }
  }
}
```

Then your agent loop is just:

```python
text = await mcp.call_tool("wait_for_utterance")    # waits for the user to speak
reply = my_agent.respond(text)                       # your LLM / logic
await mcp.call_tool("speak", {"text": reply})
```

A complete, runnable reference is **[`bridge/tools/mcp_demo.py`](../bridge/tools/mcp_demo.py)** —
it spawns the bridge and runs exactly this loop with a toy responder. Swap its
`demo_reply()` for your agent's brain and you're done.

> **Don't add clawlexa to a chat agent's shared MCP config.** `wait_for_utterance`
> *blocks*, so if it's just another server on your Slack/chat agent the model can
> call it and hang the conversation. Voice wants its **own** driver process (below).

## 5b. Production shape: the standalone voice driver

For a real always-on agent, use **[`bridge/tools/voice_agent.py`](../bridge/tools/voice_agent.py)**
instead of the demo. It spawns the bridge as its *private* MCP server (so voice
stays isolated from your other surfaces), owns the loop, drives the mood crab
(`set_state`), and routes each utterance through a **brain** — by default headless
`claude -p` run in a directory you choose, so it answers with that project's
persona/context:

```bash
.venv/bin/python tools/voice_agent.py \
  --brain-cmd /path/to/claude \
  --brain-cwd /path/to/your/agent-vault \      # its CLAUDE.md / memory load from here
  --idle-timeout 150 \
  --memory-prompt "The voice chat paused. Save a short note to memory/{date}_voice_session.md and append a line to memory/channel_voice.md. Reply: ok"
```

**Memory — so the crab remembers:**
- *Within a conversation:* the Claude brain keeps a session and `--resume`s it each
  turn, so it remembers earlier turns.
- *Across conversations:* after `--idle-timeout` seconds of silence, one final
  `--resume` turn runs your `--memory-prompt` (with write tools enabled) so the
  brain persists a session note — mirroring how a chat agent saves memory. `{date}`
  expands to `YYYY_MM_DD`. Omit `--memory-prompt` to disable.

The brain just needs its model auth in the environment (e.g. `ANTHROPIC_API_KEY`)
and, for the `claude` CLI specifically, `node` on `PATH`.

### Run it on boot (macOS LaunchAgent)

Wrap the invocation in a small launcher script (it can `export` the auth/PATH and
`cd` to the bridge), then drop a `~/Library/LaunchAgents/<your.label>.plist`:

```xml
<plist version="1.0"><dict>
  <key>Label</key><string>com.example.clawlexa-voice</string>
  <key>ProgramArguments</key>
  <array><string>/bin/sh</string><string>/path/to/run-voice-agent.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/path/to/logs/voice-agent.log</string>
  <key>StandardErrorPath</key><string>/path/to/logs/voice-agent.log</string>
</dict></plist>
```

```bash
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.example.clawlexa-voice.plist
```

It then auto-starts at login and restarts on crash. (`launchctl bootout
gui/$(id -u)/<label>` to stop it.)

## 6. Sanity check (no agent needed)

From `bridge/`, with the device on:

```bash
.venv/bin/python tools/mcp_demo.py
```

Say your wake word ("okay nabu" on the default build), then a sentence. You should
see `heard:` / `reply:` on stderr and hear the reply from the device. If that
works, your own agent will too — it's the same two tool calls.

## Notes

- **Wake word:** the default build wakes on **"okay nabu"**. To use a custom word
  (`clawlexa`, `okay iPinch`, …) train and swap a model — see
  [`training/README.md`](../training/README.md).
- **One device per bridge** for now; **trust-the-LAN** (no auth yet) — fine for a
  home network, revisit before anything hostile.

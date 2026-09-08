# clawlexa-bridge

Host-side bridge between the clawlexa ESP32 device and an MCP agent. Runs on the
same laptop as the agent (e.g. iPinch). Phase 2: a WebSocket server the device
dials into; STT/TTS and the MCP surface land in later phases.

## Setup

```bash
cd bridge
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

## Run

```bash
.venv/bin/python -m clawlexa_bridge --host 0.0.0.0 --port 8765
```

Point the firmware at this host with `idf.py menuconfig` → **clawlexa** →
`Bridge host` / `Bridge port` (the device dials `ws://<host>:<port>`). Use the
laptop's LAN IP, e.g. `192.168.1.221`.

## Test

```bash
.venv/bin/python -m pytest        # from bridge/
```

`tests/test_protocol.py` covers the pure message encode/parse; `test_server.py`
runs the real handshake over a loopback WebSocket (no device needed).

## Ambient presence greeting (Phase 6c)

The voice driver can greet you when you walk into a room, using a Home Assistant
presence sensor — no wake word needed to answer (SPEC §7a).

```bash
.venv/bin/python tools/voice_agent.py \
    --brain-cwd ~/claude --claude-cli ./node_modules/.bin/claude \
    --ha-url http://homeassistant.local:8123 \
    --ha-entity binary_sensor.study_presence
```

It needs a Home Assistant **long-lived access token** (Profile → Security →
Long-lived access tokens) in a file — `~/.config/ha-token` by default,
`--ha-token-file` to point elsewhere. The token is read at startup and never
logged. Omit `--ha-url`/`--ha-entity` and the device stays wake-word only.

To find your entity id:

```bash
curl -s -H "Authorization: Bearer $(cat ~/.config/ha-token)" \
    http://homeassistant.local:8123/api/states |
  python3 -c "import json,sys; [print(s['entity_id'], '=', s['state']) for s in json.load(sys.stdin) if 'presence' in s['entity_id']]"
```

When the room goes from clear to occupied — and it has been clear for
`--away-minutes` (default 30), outside `--quiet-hours` (default `22-8`), with no
conversation already running — the device speaks a canned time-of-day greeting
and opens a listening window. **The greeting never wakes the Claude session**:
walking past the study costs nothing. Only if you answer does the brain start,
via the normal voice loop. If you don't, the usual follow-up window times out and
the wake word re-arms.

Tuning:

| Flag | Default | What it does |
|------|---------|--------------|
| `--away-minutes` | `30` | How long the room must have been empty to earn a greeting. Also the minimum gap between two greetings. |
| `--quiet-hours` | `22-8` | Local-hour window with no greetings. `0-0` disables quiet hours. |

## Troubleshooting

**Device connects but the link fails (bridge logs `400 Bad Request`; device logs
`Error read response for Upgrade header`).** If loopback tests pass but the real
device can't complete the handshake, suspect the **macOS Application Firewall**
on the bridge host. It can let the TCP handshake through (the kernel even ACKs
the device's request) yet block the *Python process* from receiving the data, so
`recv()` raises `OSError: [Errno 57] Socket is not connected` and the server 400s
— a very misleading symptom that looks like a protocol bug but isn't.

The firewall is per-binary. Two gotchas: (1) a venv on the python.org framework
Python is a *different* binary from `/usr/bin/python3`; (2) the framework's real
interpreter is the **`Python.app`** executable inside it — allowing
`bin/python3.12` is **not** enough. Allow the right one, then **restart the
bridge** (the firewall decides per process launch):

```bash
PYAPP="$SROOT/Resources/Python.app/Contents/MacOS/Python"   # $SROOT = sys.base_prefix
# e.g. /Library/Frameworks/Python.framework/Versions/3.12/Resources/Python.app/Contents/MacOS/Python
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$PYAPP"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$PYAPP"
# then restart: python -m clawlexa_bridge ...
```

(or run the bridge from an already-allowed interpreter). Loopback always works
because it bypasses the firewall — so this only bites the real device.

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
| `--greeting` | built-in lines | `WHEN:TEXT` (WHEN = `morning`/`afternoon`/`evening`). Repeat for several lines; they rotate. A time of day you don't mention keeps its built-in line. Use it to give the device a persona without putting one in the repo. |

## Troubleshooting

**The presence greeting never fires; the log repeats `Home Assistant link to
... failed ([Errno 65] No route to host)` — but the same command works fine
from a terminal.** macOS 15 (Sequoia) added a **Local Network** privacy control,
and a `launchd` background agent can't show the permission prompt, so it is
silently denied. The symptom is confusing because it is *outbound* only: the
device's inbound WebSocket keeps working, so the crab still answers the wake
word — only Home Assistant is unreachable. Running the very same virtualenv
Python interactively (over SSH, say) succeeds, because that process has a
different responsible app.

**First confirm it really is this**, because Errno 65 reads like a routing
problem. The tell is one process doing both: the agent reaches the internet
happily (`httpx ... huggingface.co "HTTP/1.1 200 OK"` while loading STT models)
and *only* LAN addresses fail. An IPv4 literal failing exactly like the mDNS
hostname is the second tell.

Fix, and **the order matters**:

1. **Make sure the Python binary is code-signed.** This is the step that traps
   people. A `uv`-managed (or pyenv-built) CPython is often *not signed at all*
   — `codesign -dvvv .venv/bin/python` says `code object is not signed at all`.
   TCC cannot identify an unsigned binary, so it can neither prompt for it nor
   remember a grant: the request is denied in silence, no dialog appears, and no
   entry is ever added to the Local Network list. Granting the *other* "Python"
   already in that list does nothing, because it's a different binary. Sign it
   ad-hoc — the dylib as well as the executable:

   ```bash
   PYROOT=$(dirname $(dirname $(readlink -f .venv/bin/python)))
   codesign -s - --force --timestamp=none "$PYROOT/lib/"libpython*.dylib
   codesign -s - --force --timestamp=none "$PYROOT/bin/python3.12"
   codesign -dvvv "$PYROOT/bin/python3.12"   # now shows an Identifier + CDHash
   ```

2. **Restart the job**, then **approve the dialog**. Once the binary has a stable
   identity macOS can finally raise the prompt — it may appear on the machine's
   own screen rather than where you're working, so go and look at it:
   `launchctl kickstart -k gui/$(id -u)/com.ipinch.clawlexa-voice`

3. A successful start logs
   `clawlexa.presence: Home Assistant: watching <entity> on <host>`.

Running the same command in a terminal works throughout, which is a misleading
comfort: a foreground process inherits its *terminal's* local-network grant, so
success there says nothing about the launchd job.

Errno 65 here is a permissions symptom, not a routing one — don't go hunting
for a bad IP or an IPv6 problem. A quick way to tell them apart: if an IPv4
literal *and* the mDNS hostname both fail under `launchd` but both succeed in a
terminal, it's this.

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

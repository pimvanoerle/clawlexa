"""Home Assistant presence source (SPEC §7a, Phase 6c).

The IO half of the ambient greeting: subscribe to one Home Assistant entity and
yield a stream of "the room is occupied / clear" booleans for `GreetingPolicy`
to judge. Push, not polling — the greeting should land as the user walks in.

Why the raw WebSocket API rather than a Home Assistant client library: the
bridge already depends on `websockets`, the handshake is four messages, and this
keeps the bridge's dependency list short enough to install on a laptop that just
wants to talk to a crab.

We subscribe to `state_changed` and filter client-side rather than using
`subscribe_trigger`. It is the oldest, most universally available call in the
API, and on a home instance the extra traffic is a few events a second.

Everything that can be decided without a socket — the URL shape, what counts as
"occupied", how one event maps to a reading — is a pure function below, so the
protocol is host-testable and only the connection itself needs a real HA.
"""
from __future__ import annotations

import asyncio
import json
import logging
from abc import ABC, abstractmethod
from typing import Any, AsyncIterator, Callable, Optional, Sequence
from urllib.parse import urlsplit, urlunsplit

log = logging.getLogger("clawlexa.presence")

# HA state strings that mean "someone is in the room". Covers the usual presence
# entities: binary_sensor (`on`), device_tracker/person (`home`), and the
# `detected`/`occupied` wording some integrations use.
OCCUPIED_STATES = ("on", "home", "detected", "occupied", "true")

DEFAULT_RECONNECT_S = 5.0
DEFAULT_TOKEN_PATH = "~/.config/ha-token"


class PresenceSource(ABC):
    """Anything that can tell us whether a room is occupied over time.

    Keeping this abstract is what lets §7a promise HA is swappable: a webhook
    posted by an HA automation, a different home-automation system, or a fake in
    a test all satisfy it.
    """

    @abstractmethod
    def readings(self) -> AsyncIterator[bool]:
        """Yield True (occupied) / False (clear), starting with current state."""
        raise NotImplementedError


# --- pure helpers -----------------------------------------------------------

def websocket_url(base_url: str) -> str:
    """Turn a Home Assistant base URL into its WebSocket API endpoint.

    Accepts what a person would paste (`http://homeassistant.local:8123`, with or
    without a trailing slash, http or https) and returns the ws:// or wss:// API
    URL. An already-complete ws:// URL is passed through unchanged.
    """
    parts = urlsplit(base_url.strip())
    if not parts.netloc:
        raise ValueError(f"Home Assistant URL needs a host: {base_url!r}")
    scheme = {"http": "ws", "https": "wss", "ws": "ws", "wss": "wss"}.get(parts.scheme)
    if scheme is None:
        raise ValueError(f"unsupported scheme in {base_url!r} (want http/https)")
    path = parts.path.rstrip("/")
    if not path.endswith("/api/websocket"):
        path += "/api/websocket"
    return urlunsplit((scheme, parts.netloc, path, "", ""))


def state_is_occupied(state: Optional[str],
                      occupied_states: Sequence[str] = OCCUPIED_STATES) -> Optional[bool]:
    """Map an HA state string to a presence reading.

    Returns None for the states that mean "the sensor can't say" (`unavailable`,
    `unknown`, missing) — those must not be read as "the room is empty", or a
    sensor blipping offline would fake a 30-minute absence and greet you when it
    came back.
    """
    if state is None:
        return None
    s = state.strip().lower()
    if s in ("unavailable", "unknown", "none", ""):
        return None
    return s in occupied_states


def reading_from_event(msg: dict[str, Any], entity_id: str,
                       occupied_states: Sequence[str] = OCCUPIED_STATES) -> Optional[bool]:
    """Extract our entity's presence reading from a `state_changed` event, or
    None if the message is about something else / says nothing useful."""
    if msg.get("type") != "event":
        return None
    event = msg.get("event") or {}
    if event.get("event_type") != "state_changed":
        return None
    data = event.get("data") or {}
    if data.get("entity_id") != entity_id:
        return None
    new_state = data.get("new_state") or {}
    return state_is_occupied(new_state.get("state"), occupied_states)


def reading_from_states(states: Any, entity_id: str,
                        occupied_states: Sequence[str] = OCCUPIED_STATES) -> Optional[bool]:
    """Find our entity in a `get_states` result and read its current state."""
    if not isinstance(states, list):
        return None
    for st in states:
        if isinstance(st, dict) and st.get("entity_id") == entity_id:
            return state_is_occupied(st.get("state"), occupied_states)
    return None


def read_token(path: str) -> str:
    """Read a long-lived access token from a file. Never logged, never echoed."""
    import os
    with open(os.path.expanduser(path), "r") as f:
        token = f.read().strip()
    if not token:
        raise ValueError(f"no token in {path}")
    return token


# --- the connection ---------------------------------------------------------

class HomeAssistantPresence(PresenceSource):
    """Subscribe to one HA entity and yield presence readings, reconnecting for
    as long as the caller keeps iterating.

    `connect` is injectable so the whole handshake — auth, get_states,
    subscribe, event decoding — is tested against a fake socket.
    """

    def __init__(self, base_url: str, token: str, entity_id: str, *,
                 occupied_states: Sequence[str] = OCCUPIED_STATES,
                 reconnect_s: float = DEFAULT_RECONNECT_S,
                 connect: Optional[Callable[[str], Any]] = None) -> None:
        self._url = websocket_url(base_url)
        self._token = token
        self._entity_id = entity_id
        self._occupied_states = tuple(occupied_states)
        self._reconnect_s = reconnect_s
        self._connect = connect
        # Redacted host:port for logs — the token must never appear in one.
        self._where = urlsplit(self._url).netloc

    async def readings(self) -> AsyncIterator[bool]:
        while True:
            try:
                async for reading in self._session():
                    yield reading
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # a home server, a laptop lid, a flaky VLAN
                log.warning("Home Assistant link to %s failed (%s); retrying in %.0fs",
                            self._where, exc, self._reconnect_s)
            else:
                log.info("Home Assistant closed the connection; reconnecting")
            await asyncio.sleep(self._reconnect_s)

    async def _session(self) -> AsyncIterator[bool]:
        connect = self._connect
        if connect is None:
            import websockets
            connect = websockets.connect
        async with connect(self._url) as ws:
            await self._authenticate(ws)
            log.info("Home Assistant: watching %s on %s", self._entity_id, self._where)
            await ws.send(json.dumps({"id": 1, "type": "get_states"}))
            await ws.send(json.dumps({"id": 2, "type": "subscribe_events",
                                      "event_type": "state_changed"}))
            async for raw in ws:
                msg = json.loads(raw)
                if msg.get("type") == "result" and msg.get("id") == 1:
                    # Current state, so the policy starts with a baseline. Its
                    # first-reading rule means this can never itself greet.
                    initial = reading_from_states(msg.get("result"), self._entity_id,
                                                  self._occupied_states)
                    if initial is None:
                        log.warning("entity %s not found (or unavailable) in Home "
                                    "Assistant — check the entity id", self._entity_id)
                    else:
                        yield initial
                    continue
                if msg.get("type") == "result" and not msg.get("success", True):
                    raise RuntimeError(f"Home Assistant rejected a call: {msg.get('error')}")
                reading = reading_from_event(msg, self._entity_id, self._occupied_states)
                if reading is not None:
                    yield reading

    async def _authenticate(self, ws) -> None:
        """auth_required -> auth -> auth_ok. Anything else is fatal for this
        session; the caller's reconnect loop decides what to do about it."""
        hello = json.loads(await ws.recv())
        if hello.get("type") != "auth_required":
            raise RuntimeError(f"unexpected greeting from Home Assistant: {hello.get('type')!r}")
        await ws.send(json.dumps({"type": "auth", "access_token": self._token}))
        reply = json.loads(await ws.recv())
        if reply.get("type") != "auth_ok":
            raise RuntimeError("Home Assistant rejected the access token "
                               f"({reply.get('message') or reply.get('type')})")

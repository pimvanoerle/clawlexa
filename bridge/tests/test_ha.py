"""Unit tests for the Home Assistant presence source (SPEC §7a, Phase 6c).

The pure decoding helpers, plus the whole handshake driven against a fake
socket — auth, get_states, subscribe, event decoding — so the protocol is
covered without a Home Assistant instance.
"""
import asyncio
import json

import pytest

from clawlexa_bridge.ha import (HomeAssistantPresence, reading_from_event,
                                reading_from_states, state_is_occupied,
                                websocket_url)

ENTITY = "binary_sensor.study_presence"


# --- pure helpers -----------------------------------------------------------

def test_websocket_url_from_what_a_person_would_paste():
    assert websocket_url("http://homeassistant.local:8123") == \
        "ws://homeassistant.local:8123/api/websocket"
    assert websocket_url("http://homeassistant.local:8123/") == \
        "ws://homeassistant.local:8123/api/websocket"
    assert websocket_url("https://ha.example.com") == \
        "wss://ha.example.com/api/websocket"


def test_websocket_url_passes_through_a_complete_url():
    url = "ws://10.0.0.4:8123/api/websocket"
    assert websocket_url(url) == url


def test_websocket_url_rejects_nonsense():
    with pytest.raises(ValueError):
        websocket_url("homeassistant.local:8123")   # no scheme -> no host parsed
    with pytest.raises(ValueError):
        websocket_url("ftp://homeassistant.local")


def test_state_is_occupied_covers_the_usual_entity_flavours():
    assert state_is_occupied("on") and state_is_occupied("home")
    assert state_is_occupied("detected") and state_is_occupied("occupied")
    assert state_is_occupied("off") is False
    assert state_is_occupied("not_home") is False


def test_unavailable_states_are_unknown_not_empty():
    """A sensor dropping offline must not read as 'the room is empty' — that
    would fake an absence and greet the user when it came back."""
    assert state_is_occupied("unavailable") is None
    assert state_is_occupied("unknown") is None
    assert state_is_occupied(None) is None


def test_reading_from_event_ignores_other_entities_and_event_types():
    def event(entity, state):
        return {"type": "event", "event": {"event_type": "state_changed",
                                           "data": {"entity_id": entity,
                                                    "new_state": {"state": state}}}}
    assert reading_from_event(event(ENTITY, "on"), ENTITY) is True
    assert reading_from_event(event("light.kitchen", "on"), ENTITY) is None
    assert reading_from_event({"type": "result", "success": True}, ENTITY) is None
    assert reading_from_event({"type": "event", "event": {"event_type": "call_service"}},
                              ENTITY) is None


def test_reading_from_states_finds_the_entity():
    states = [{"entity_id": "light.kitchen", "state": "on"},
              {"entity_id": ENTITY, "state": "off"}]
    assert reading_from_states(states, ENTITY) is False
    assert reading_from_states(states, "binary_sensor.missing") is None
    assert reading_from_states("not a list", ENTITY) is None


# --- the handshake, against a fake socket -----------------------------------

class FakeWS:
    """A scripted Home Assistant WebSocket: `recv` pops the script, iteration
    yields whatever is left, and sends are recorded."""

    def __init__(self, script):
        self._script = [json.dumps(m) for m in script]
        self.sent = []

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False

    async def recv(self):
        return self._script.pop(0)

    async def send(self, raw):
        self.sent.append(json.loads(raw))

    def __aiter__(self):
        async def gen():
            while self._script:
                yield self._script.pop(0)
        return gen()


def event(entity, state):
    return {"type": "event", "event": {"event_type": "state_changed",
                                       "data": {"entity_id": entity,
                                                "new_state": {"state": state}}}}


def collect(script, n, entity=ENTITY):
    """Run one session against `script` and return the first `n` readings."""
    ws = FakeWS(script)
    src = HomeAssistantPresence("http://ha.local:8123", "tok", entity,
                                connect=lambda url: ws)
    async def run():
        out = []
        async for reading in src._session():
            out.append(reading)
            if len(out) == n:
                break
        return out, ws
    return asyncio.run(run())


def test_session_authenticates_then_subscribes():
    readings, ws = collect([
        {"type": "auth_required"},
        {"type": "auth_ok"},
        {"type": "result", "id": 1, "success": True,
         "result": [{"entity_id": ENTITY, "state": "off"}]},
        event(ENTITY, "on"),
    ], n=2)
    assert readings == [False, True]           # baseline, then the arrival
    assert ws.sent[0]["type"] == "auth"        # token goes out first
    assert ws.sent[0]["access_token"] == "tok"
    assert [m["type"] for m in ws.sent[1:]] == ["get_states", "subscribe_events"]
    assert ws.sent[2]["event_type"] == "state_changed"


def test_session_rejects_a_bad_token():
    ws = FakeWS([{"type": "auth_required"},
                 {"type": "auth_invalid", "message": "Invalid access token"}])
    src = HomeAssistantPresence("http://ha.local:8123", "bad", ENTITY,
                                connect=lambda url: ws)
    async def run():
        async for _ in src._session():
            pass
    with pytest.raises(RuntimeError, match="rejected the access token"):
        asyncio.run(run())


def test_session_ignores_traffic_for_other_entities():
    readings, _ = collect([
        {"type": "auth_required"},
        {"type": "auth_ok"},
        {"type": "result", "id": 1, "success": True,
         "result": [{"entity_id": ENTITY, "state": "off"}]},
        event("light.kitchen", "on"),
        event("sensor.study_temperature", "21"),
        event(ENTITY, "on"),
    ], n=2)
    assert readings == [False, True]


def test_missing_entity_yields_no_baseline_but_keeps_running():
    """A typo'd entity id shouldn't crash the bridge — it warns and waits."""
    readings, _ = collect([
        {"type": "auth_required"},
        {"type": "auth_ok"},
        {"type": "result", "id": 1, "success": True, "result": []},
        event(ENTITY, "on"),
    ], n=1)
    assert readings == [True]

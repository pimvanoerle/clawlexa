import json

import pytest

from clawlexa_bridge.protocol import (
    PROTOCOL_VERSION,
    parse_message,
    play_begin_message,
    play_end_message,
    welcome_message,
)


def test_welcome_is_valid_json():
    msg = json.loads(welcome_message())
    assert msg["type"] == "welcome"
    assert msg["v"] == PROTOCOL_VERSION


def test_play_begin_carries_format():
    msg = json.loads(play_begin_message(16000, 1, 16))
    assert msg["type"] == "play_begin"
    assert (msg["rate"], msg["channels"], msg["bits"]) == (16000, 1, 16)
    assert "ms" not in msg  # omitted when not given


def test_play_begin_carries_duration():
    msg = json.loads(play_begin_message(16000, 1, 16, ms=1234))
    assert msg["ms"] == 1234  # so the device mutes its mic for the whole reply


def test_play_end():
    assert json.loads(play_end_message())["type"] == "play_end"


def test_set_state_message_valid():
    from clawlexa_bridge.protocol import set_state_message
    msg = json.loads(set_state_message("listening"))
    assert msg["type"] == "set_state" and msg["state"] == "listening"


def test_set_state_message_rejects_unknown_state():
    from clawlexa_bridge.protocol import set_state_message
    with pytest.raises(ValueError):
        set_state_message("dancing")


def test_show_message_carries_text():
    from clawlexa_bridge.protocol import show_message
    msg = json.loads(show_message("hi there"))
    assert msg["type"] == "show" and msg["text"] == "hi there"


def test_parse_valid_hello():
    msg = parse_message('{"type": "hello", "device": "clawlexa"}')
    assert msg["type"] == "hello"
    assert msg["device"] == "clawlexa"


def test_parse_rejects_bad_json():
    with pytest.raises(ValueError):
        parse_message("not json at all")


def test_parse_rejects_non_object():
    with pytest.raises(ValueError):
        parse_message("[1, 2, 3]")


def test_parse_rejects_missing_type():
    with pytest.raises(ValueError):
        parse_message('{"device": "clawlexa"}')

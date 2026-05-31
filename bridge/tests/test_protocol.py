import json

import pytest

from clawlexa_bridge.protocol import PROTOCOL_VERSION, parse_message, welcome_message


def test_welcome_is_valid_json():
    msg = json.loads(welcome_message())
    assert msg["type"] == "welcome"
    assert msg["v"] == PROTOCOL_VERSION


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

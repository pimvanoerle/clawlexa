"""Wire protocol for the device <-> bridge WebSocket link (v1).

Control messages are JSON **text** frames, each an object with a "type" field.
Audio will travel as raw binary frames (Phase 2c). Keeping the encode/parse
logic here (pure, no I/O) makes it unit-testable without a socket or a device.
"""
from __future__ import annotations

import json
from typing import Any

PROTOCOL_VERSION = 1


def welcome_message() -> str:
    """The bridge's reply to a device 'hello'."""
    return json.dumps(
        {"type": "welcome", "bridge": "clawlexa-bridge", "v": PROTOCOL_VERSION}
    )


def play_begin_message(rate: int, channels: int, bits: int) -> str:
    """Tell the device a run of binary PCM frames (to play) is starting."""
    return json.dumps(
        {"type": "play_begin", "rate": rate, "channels": channels, "bits": bits}
    )


def play_end_message() -> str:
    return json.dumps({"type": "play_end"})


def parse_message(raw: str) -> dict[str, Any]:
    """Parse a JSON control frame into a dict.

    Raises ValueError if it isn't valid JSON, isn't an object, or has no 'type'.
    """
    try:
        msg = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid JSON: {exc}") from exc
    if not isinstance(msg, dict):
        raise ValueError("control message must be a JSON object")
    if "type" not in msg:
        raise ValueError("control message missing 'type'")
    return msg

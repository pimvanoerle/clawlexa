"""MCP server surface (Phase 5) — the device's voice loop as MCP tools.

An agent (iPinch, Claude Desktop, ...) drives the device through these tools
instead of the bridge's standalone "You said: X" echo:
  - wait_for_utterance(): block until the user speaks (after the wake word fires)
  - speak(text): say something back on the device

The tools operate on a Hub shared with the WebSocket server; both run in one
process on one event loop. v1 transport is stdio (how an agent typically adds an
MCP server). Keeping the tools as thin wrappers over the Hub means the agent loop
is exercised by the Hub's unit tests, not the transport.
"""
from __future__ import annotations

import asyncio
import logging

from mcp.server.fastmcp import FastMCP

from .hub import Hub
from .server import send_wav, serve
from .stt import STT, WhisperSTT
from .tts import TTS, PiperTTS

log = logging.getLogger("clawlexa.bridge")


def build_mcp(hub: Hub) -> FastMCP:
    mcp = FastMCP("clawlexa")

    @mcp.tool()
    async def wait_for_utterance(timeout_ms: int | None = None) -> str:
        """Wait until the user speaks to the clawlexa device (after its on-device
        wake word fires) and return the transcript. Blocks until something is
        said; returns an empty string if `timeout_ms` elapses first."""
        timeout = timeout_ms / 1000 if timeout_ms else None
        try:
            return await hub.next_utterance(timeout)
        except asyncio.TimeoutError:
            return ""

    @mcp.tool()
    async def speak(text: str) -> str:
        """Speak `text` aloud on the clawlexa device (text-to-speech)."""
        await hub.speak(text)
        return "ok"

    @mcp.tool()
    async def set_state(state: str) -> str:
        """Set the device's ambient status indicator. One of:
        idle | listening | thinking | speaking | error."""
        await hub.set_state(state)
        return "ok"

    @mcp.tool()
    async def show(text: str) -> str:
        """Show a short line of text on the device's screen."""
        await hub.show(text)
        return "ok"

    return mcp


async def run_mcp(host: str, port: int, stt: STT | None = None,
                  tts: TTS | None = None) -> None:
    """Run the device WebSocket server and the MCP (stdio) server together,
    sharing one Hub. Models load before the stdio protocol starts so their
    output can't corrupt it."""
    if stt is None:
        log.info("loading STT model (faster-whisper)...")
        stt = WhisperSTT()
    if tts is None:
        log.info("loading TTS voice (piper)...")
        tts = PiperTTS()
    hub = Hub(tts, send_wav)
    mcp = build_mcp(hub)
    log.info("clawlexa MCP server ready (stdio); device link on ws://%s:%d", host, port)
    await asyncio.gather(
        serve(host, port, stt=stt, tts=tts, hub=hub),
        mcp.run_stdio_async(),
    )

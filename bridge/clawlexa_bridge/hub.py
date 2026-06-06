"""The bridge between the device link and the MCP agent (Phase 5).

The WebSocket side feeds transcribed utterances in (`submit_utterance`) and the
MCP side pulls them out (`next_utterance`) and pushes speech back (`speak`).
Keeping this state in one small, IO-free-ish object makes the agent loop
unit-testable with fakes — no MCP transport, no real device.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Awaitable, Callable

from .tts import TTS

log = logging.getLogger("clawlexa.bridge")


class Hub:
    def __init__(self, tts: TTS, send_wav: Callable[[object, str], Awaitable[None]]) -> None:
        self._tts = tts
        self._send_wav = send_wav        # async (ws, wav_path) -> None
        self._ws = None                  # the active device connection, if any
        self._utterances: asyncio.Queue[str] = asyncio.Queue()

    # --- device link side ----------------------------------------------------
    def attach(self, ws) -> None:
        self._ws = ws

    def detach(self, ws) -> None:
        if self._ws is ws:
            self._ws = None

    @property
    def device_connected(self) -> bool:
        return self._ws is not None

    async def submit_utterance(self, text: str) -> None:
        """A transcript the device captured — hand it to the waiting agent."""
        await self._utterances.put(text)

    # --- MCP agent side ------------------------------------------------------
    async def next_utterance(self, timeout: float | None = None) -> str:
        """Block until the user speaks (after the wake word). Raises
        asyncio.TimeoutError if `timeout` (seconds) elapses first."""
        if timeout is not None:
            return await asyncio.wait_for(self._utterances.get(), timeout)
        return await self._utterances.get()

    async def speak(self, text: str) -> None:
        """Synthesize `text` and play it on the device."""
        ws = self._ws
        if ws is None:
            raise RuntimeError("no device connected")
        wav = await asyncio.to_thread(self._tts.synthesize, text)
        await self._send_wav(ws, wav)
        log.info('spoke: "%s"', text)

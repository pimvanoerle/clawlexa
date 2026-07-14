"""The bridge between the device link and the MCP agent (Phase 5).

The WebSocket side feeds transcribed utterances in (`submit_utterance`) and the
MCP side pulls them out (`next_utterance`) and pushes speech back (`speak`).
Keeping this state in one small, IO-free-ish object makes the agent loop
unit-testable with fakes — no MCP transport, no real device.
"""
from __future__ import annotations

import asyncio
import logging
import wave
from typing import Awaitable, Callable

from .protocol import set_state_message, show_message
from .tts import TTS

log = logging.getLogger("clawlexa.bridge")


class Hub:
    def __init__(self, tts: TTS, send_wav: Callable[[object, str], Awaitable[None]]) -> None:
        self._tts = tts
        self._send_wav = send_wav        # async (ws, wav_path) -> None
        self._ws = None                  # the active device connection, if any
        self._conv = None                # the connection's Conversation, if any
        self._utterances: asyncio.Queue[str] = asyncio.Queue()

    # --- device link side ----------------------------------------------------
    def attach(self, ws, conversation=None) -> None:
        self._ws = ws
        self._conv = conversation

    def detach(self, ws) -> None:
        if self._ws is ws:
            self._ws = None
            self._conv = None

    @property
    def device_connected(self) -> bool:
        return self._ws is not None

    async def submit_utterance(self, text: str) -> None:
        """A transcript the device captured — hand it to the waiting agent."""
        await self._utterances.put(text)

    # --- MCP agent side ------------------------------------------------------
    async def next_utterance(self, timeout: float | None = None) -> str:
        """Block until the user speaks, then drain any utterances that piled up
        while the agent was busy and coalesce them into one — so the agent answers
        what was *just* said rather than working through a growing backlog of stale
        snippets (SPEC §7). Raises asyncio.TimeoutError if `timeout` (seconds)
        elapses before the first utterance."""
        if timeout is not None:
            first = await asyncio.wait_for(self._utterances.get(), timeout)
        else:
            first = await self._utterances.get()
        parts = [first]
        while True:  # sweep up everything else already waiting
            try:
                parts.append(self._utterances.get_nowait())
            except asyncio.QueueEmpty:
                break
        if len(parts) > 1:
            log.info("coalesced %d backlogged utterances", len(parts))
        return " ".join(p.strip() for p in parts if p.strip())

    async def speak(self, text: str) -> None:
        """Synthesize `text`, play it on the device, and return once it has
        ~finished playing — so the agent stays 'speaking' for the whole clip and
        doesn't re-listen over its own voice (half-duplex)."""
        ws = self._require_ws()
        wav = await asyncio.to_thread(self._tts.synthesize, text)
        # Bracket the reply so the conversation window doesn't re-arm mid-speech
        # and, once done, starts the follow-up silence timer (SPEC §7).
        if self._conv is not None:
            self._conv.reply_started()
        await self._send_wav(ws, wav)
        with wave.open(wav, "rb") as w:
            play_s = w.getnframes() / w.getframerate()
        await asyncio.sleep(play_s)
        if self._conv is not None:
            self._conv.reply_finished(has_more=not self._utterances.empty())
        log.info('spoke: "%s"', text)

    async def end_conversation(self) -> None:
        """The agent signalled the conversation is over (a goodbye) — end it now so
        the device re-arms its wake word instead of waiting out the follow-up
        window. The server's watchdog sends the actual end_turn on its next tick."""
        if self._conv is not None:
            self._conv.end_now()
        log.info("end_conversation requested by agent")

    async def set_state(self, state: str) -> None:
        """Set the device's ambient status indicator (SPEC §8)."""
        await self._require_ws().send(set_state_message(state))  # raises ValueError on bad state
        log.info("set_state: %s", state)

    async def show(self, text: str) -> None:
        """Push a short line of text to the device's screen."""
        await self._require_ws().send(show_message(text))
        log.info('show: "%s"', text)

    def _require_ws(self):
        if self._ws is None:
            raise RuntimeError("no device connected")
        return self._ws

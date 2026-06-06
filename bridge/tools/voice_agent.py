#!/usr/bin/env python3
"""Standalone voice driver for the clawlexa bridge.

This is the real-agent counterpart to ``mcp_demo.py``: it spawns the bridge as
an MCP server over stdio, owns the device's voice loop, and routes each spoken
utterance through a *brain* — by default a **warm** Claude Agent SDK session
(``claude-agent-sdk``) running in your agent's vault dir, so it answers with that
agent's persona/context and remembers the conversation.

Why a standalone process instead of an entry in the agent's MCP config? The
bridge's ``wait_for_utterance`` *blocks* until someone speaks. If it were just
another MCP server on a chat agent (e.g. iPinch's Slack loop), the model could
call it mid-conversation and hang. Owning the loop here keeps voice isolated:
the bridge is this process's private MCP server and never leaks into other
surfaces.

The loop, per turn:

    idle  ->  wait_for_utterance  ->  thinking  ->  brain.reply(text)  ->  speaking  ->  speak

Memory + speed (so the crab remembers, and follow-ups feel instant):
  - The brain keeps **one warm Claude session** across turns, so it remembers the
    conversation and only the first turn pays cold-start latency; later turns are
    just an LLM round-trip.
  - After ``--idle-timeout`` seconds of silence the session is closed (freeing the
    warm process). If a ``--memory-prompt`` is set, one final turn first asks the
    brain to save a session note — mirroring how a chat agent persists memory.

Run from the bridge/ directory (device powered + on WiFi; nothing else bound to
the bridge's WS port):

    .venv/bin/python tools/voice_agent.py --brain-cwd ~/claude \
        --claude-cli ./node_modules/.bin/claude \
        --memory-prompt "Save a short note of this voice chat to memory/{date}_voice.md. Reply: ok"

Everything prints to stderr — stdout belongs to the MCP stdio protocol.
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from abc import ABC, abstractmethod
from datetime import date
from typing import Callable, Optional, Sequence

log = logging.getLogger("clawlexa.voice")

# Spoken replies, not chat: keep the brain terse and TTS-friendly.
VOICE_SYSTEM_PROMPT = (
    "You are a voice assistant speaking through a small speaker. Reply in one or "
    "two short, natural spoken sentences. Do not use markdown, lists, code blocks, "
    "URLs, or emoji — your reply is read aloud by text-to-speech."
)
BRAIN_ERROR_REPLY = "Sorry, I hit a problem thinking about that."
EMPTY_BRAIN_REPLY = "I didn't catch that — could you say it again?"


class BrainError(RuntimeError):
    """The brain failed to produce a reply (couldn't start, errored, timed out)."""


# --- the brain: transcript in, spoken reply out -----------------------------
class Brain(ABC):
    @abstractmethod
    async def reply(self, transcript: str) -> str:
        """Answer one utterance (and remember it for the rest of the conversation)."""

    async def end_session(self) -> None:
        """Conversation went idle — persist memory and/or tear down. Default: nothing."""
        return None


class ClaudeSessionBrain(Brain):
    """A brain backed by a **warm** Claude Agent SDK session. The session is
    opened lazily on the first utterance and kept alive across turns (so it
    remembers the conversation and follow-ups skip cold-start), then closed by
    ``end_session`` when the conversation goes idle — at which point it optionally
    runs one more turn to save a memory note. Running it with ``cwd`` set to your
    agent's vault gives it that agent's CLAUDE.md / persona / memory.

    `client_factory` is injectable for tests; by default it builds a real
    ClaudeSDKClient (imported lazily so the dependency is optional)."""

    def __init__(self, cwd: Optional[str] = None, cli_path: Optional[str] = None,
                 system_prompt: str = VOICE_SYSTEM_PROMPT, timeout: float = 120.0,
                 memory_prompt: Optional[str] = None,
                 permission_mode: str = "acceptEdits",
                 setting_sources: Sequence[str] = ("project", "user"),
                 client_factory: Optional[Callable[[], object]] = None) -> None:
        self._cwd = cwd
        self._cli_path = cli_path
        self._system_prompt = system_prompt
        self._timeout = timeout
        self._memory_prompt = memory_prompt
        self._permission_mode = permission_mode
        self._setting_sources = tuple(setting_sources)
        self._client_factory = client_factory or self._default_factory
        self._client = None
        self._turns = 0  # turns this conversation, reset when the session closes

    def _default_factory(self):
        try:
            from claude_agent_sdk import ClaudeAgentOptions, ClaudeSDKClient
        except ImportError as e:  # optional dep — only needed for this brain
            raise BrainError("claude-agent-sdk is not installed "
                             "(pip install claude-agent-sdk)") from e
        opts = ClaudeAgentOptions(
            cwd=self._cwd,
            cli_path=self._cli_path,
            system_prompt={"type": "preset", "preset": "claude_code",
                           "append": self._system_prompt},
            setting_sources=list(self._setting_sources),
            permission_mode=self._permission_mode,
        )
        return ClaudeSDKClient(options=opts)

    async def _ensure(self) -> None:
        if self._client is None:
            client = self._client_factory()  # may raise BrainError (missing dep)
            await client.connect()
            self._client = client

    async def _drain(self) -> str:
        """Collect the assistant's spoken text from one response; raise BrainError
        if the model reports an error (auth/billing/etc.)."""
        parts = []
        async for msg in self._client.receive_response():
            if type(msg).__name__ == "AssistantMessage":
                err = getattr(msg, "error", None)
                if err:
                    raise BrainError(f"brain error: {err}")
                for block in getattr(msg, "content", None) or []:
                    if type(block).__name__ == "TextBlock":
                        parts.append(getattr(block, "text", ""))
        return "".join(parts).strip()

    async def reply(self, transcript: str) -> str:
        try:
            await self._ensure()
            await asyncio.wait_for(self._client.query(transcript), self._timeout)
            out = await asyncio.wait_for(self._drain(), self._timeout)
        except BrainError:
            await self._safe_close()  # start the next turn from a fresh session
            raise
        except (asyncio.TimeoutError, Exception) as e:
            await self._safe_close()
            raise BrainError(str(e) or "brain failed") from e
        self._turns += 1
        return out

    async def end_session(self) -> None:
        try:
            if self._client is not None and self._memory_prompt and self._turns > 0:
                prompt = self._memory_prompt.replace(
                    "{date}", date.today().strftime("%Y_%m_%d"))
                log.info("saving voice session memory (%d turns)...", self._turns)
                await asyncio.wait_for(self._client.query(prompt), self._timeout)
                await asyncio.wait_for(self._drain(), self._timeout)
        except Exception as e:
            log.warning("memory save failed: %s", e)
        finally:
            await self._safe_close()
            self._turns = 0

    async def _safe_close(self) -> None:
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:
                pass
            self._client = None


# --- the device IO surface (so the loop is testable without a real device) --
class VoiceIO(ABC):
    """The four bridge tools the loop needs, as an interface — an MCP-backed
    impl drives the real device, a fake drives the tests."""

    @abstractmethod
    async def wait_for_utterance(self, timeout_s: Optional[float] = None) -> str: ...
    @abstractmethod
    async def set_state(self, state: str) -> None: ...
    @abstractmethod
    async def speak(self, text: str) -> None: ...
    @abstractmethod
    async def show(self, text: str) -> None: ...


class McpVoiceIO(VoiceIO):
    """VoiceIO backed by an MCP ClientSession talking to the bridge."""

    def __init__(self, session) -> None:
        self._session = session

    @staticmethod
    def _text(result) -> str:
        return result.content[0].text if result.content else ""

    async def wait_for_utterance(self, timeout_s: Optional[float] = None) -> str:
        args = {"timeout_ms": int(timeout_s * 1000)} if timeout_s else {}
        return self._text(await self._session.call_tool("wait_for_utterance", args))

    async def set_state(self, state: str) -> None:
        await self._session.call_tool("set_state", {"state": state})

    async def speak(self, text: str) -> None:
        await self._session.call_tool("speak", {"text": text})

    async def show(self, text: str) -> None:
        await self._session.call_tool("show", {"text": text})


# --- the loop ---------------------------------------------------------------
async def run_voice_loop(io: VoiceIO, brain: Brain, *, idle_timeout_s: float = 1800.0,
                         max_turns: Optional[int] = None) -> None:
    """Drive the device voice loop through `brain`. The brain's session stays warm
    across turns; after `idle_timeout_s` of silence `end_session` runs (persist
    memory + close the warm session), so the next utterance opens a fresh one. Set
    `idle_timeout_s` <= 0 to keep the session open forever. A brain failure is
    handled per-turn (error crab + spoken apology) so one bad turn doesn't kill the
    loop. `max_turns` (for tests) stops after that many utterances."""
    timeout = idle_timeout_s if idle_timeout_s and idle_timeout_s > 0 else None
    turn = 0
    while max_turns is None or turn < max_turns:
        await io.set_state("idle")
        text = await io.wait_for_utterance(timeout)
        if not text:
            await brain.end_session()  # idle: persist memory + drop the warm session
            continue
        turn += 1
        log.info("heard: %r", text)
        await io.set_state("thinking")
        try:
            reply = await brain.reply(text)
        except BrainError as e:
            log.warning("brain error: %s", e)
            await io.set_state("error")
            await io.speak(BRAIN_ERROR_REPLY)
            continue
        if not reply:
            reply = EMPTY_BRAIN_REPLY
        log.info("reply: %r", reply)
        await io.set_state("speaking")
        await io.speak(reply)


async def _serve(brain: Brain, host: str, port: int, idle_timeout_s: float) -> None:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    # Spawn the bridge as our private MCP server (same shape an agent's MCP
    # config would use), passing the device-link host/port through.
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "clawlexa_bridge", "--mcp", "--host", host, "--port", str(port)],
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await session.list_tools()
            log.info("connected; bridge tools: %s", [t.name for t in tools.tools])
            log.info("waiting for the wake word / tap — Ctrl-C to quit")
            try:
                await run_voice_loop(McpVoiceIO(session), brain,
                                     idle_timeout_s=idle_timeout_s)
            finally:
                await brain.end_session()  # save memory + close on shutdown


def main() -> None:
    parser = argparse.ArgumentParser(prog="voice_agent")
    parser.add_argument("--brain-cwd", default=None,
                        help="working dir for the brain — point at your agent's "
                             "vault/project dir so it inherits that context/persona")
    parser.add_argument("--claude-cli", default=None,
                        help="path to the Claude Code CLI the Agent SDK should drive "
                             "(default: whatever 'claude' resolves to on PATH)")
    parser.add_argument("--brain-timeout", type=float, default=120.0,
                        help="seconds to wait for the brain per turn (default: 120)")
    parser.add_argument("--idle-timeout", type=float, default=1800.0,
                        help="seconds of silence that ends a conversation: closes the "
                             "warm session and triggers a memory save (default: 1800 = "
                             "30 min); <=0 to keep the session open forever")
    parser.add_argument("--memory-prompt", default=None,
                        help="when a conversation goes idle, this is sent to the brain "
                             "(in its warm session) to save memory. '{date}' expands to "
                             "YYYY_MM_DD. Omit to disable.")
    parser.add_argument("--host", default="0.0.0.0", help="device-link bind address")
    parser.add_argument("--port", type=int, default=8765, help="device-link port")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
                        stream=sys.stderr)
    brain = ClaudeSessionBrain(cwd=args.brain_cwd, cli_path=args.claude_cli,
                               timeout=args.brain_timeout, memory_prompt=args.memory_prompt)
    try:
        asyncio.run(_serve(brain, args.host, args.port, args.idle_timeout))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

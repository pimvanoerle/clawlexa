#!/usr/bin/env python3
"""Standalone voice driver for the clawlexa bridge.

This is the real-agent counterpart to ``mcp_demo.py``: it spawns the bridge as
an MCP server over stdio, owns the device's voice loop, and routes each spoken
utterance through a *brain* — by default headless ``claude -p`` running in your
agent's project/vault directory, so it answers with that agent's persona/context.

Why a standalone process instead of an entry in the agent's MCP config? The
bridge's ``wait_for_utterance`` *blocks* until someone speaks. If it were just
another MCP server on a chat agent (e.g. iPinch's Slack loop), the model could
call it mid-conversation and hang. Owning the loop here keeps voice isolated:
the bridge is this process's private MCP server and never leaks into other
surfaces.

The loop, per turn:

    idle  ->  wait_for_utterance  ->  thinking  ->  brain.reply(text)  ->  speaking  ->  speak

Memory (so the crab remembers):
  - Within a conversation, the Claude brain keeps a session and ``--resume``s it
    each turn, so it remembers earlier turns.
  - When the conversation goes idle (``--idle-timeout`` seconds of silence) and a
    ``--memory-prompt`` is set, one final ``--resume`` turn asks the brain to save
    a session note — mirroring how a chat agent persists memory.

Run from the bridge/ directory (device powered + on WiFi; nothing else bound to
the bridge's WS port):

    .venv/bin/python tools/voice_agent.py --brain-cwd ~/claude \
        --memory-prompt "Save a short note of this voice chat to memory/{date}_voice.md. Reply: ok"

Everything prints to stderr — stdout belongs to the MCP stdio protocol.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import subprocess
import sys
from abc import ABC, abstractmethod
from datetime import date
from typing import Optional, Sequence

log = logging.getLogger("clawlexa.voice")

# Spoken replies, not chat: keep the brain terse and TTS-friendly.
VOICE_SYSTEM_PROMPT = (
    "You are a voice assistant speaking through a small speaker. Reply in one or "
    "two short, natural spoken sentences. Do not use markdown, lists, code blocks, "
    "URLs, or emoji — your reply is read aloud by text-to-speech."
)
BRAIN_ERROR_REPLY = "Sorry, I hit a problem thinking about that."
EMPTY_BRAIN_REPLY = "I didn't catch that — could you say it again?"
# Tools the brain may use unprompted when saving memory (read its vault + write notes).
MEMORY_TOOLS = ("Read", "Edit", "Write", "Glob", "Grep")


class BrainError(RuntimeError):
    """The brain failed to produce a reply (non-zero exit, timeout, bad output)."""


# --- the brain: transcript in, spoken reply out -----------------------------
class Brain(ABC):
    @abstractmethod
    def reply(self, transcript: str) -> str:
        """Answer one utterance (and remember it for the rest of the conversation)."""

    def end_session(self) -> None:
        """Called when the conversation goes idle — persist memory and/or reset.
        Default: nothing."""
        return None


class ClaudeBrain(Brain):
    """A brain backed by headless ``claude -p``. Running it in your agent's
    vault dir (``cwd``) gives it that agent's CLAUDE.md / persona / memory. Keeps
    a Claude session across turns via ``--resume`` so it remembers the
    conversation; on ``end_session`` it optionally runs one more turn to save a
    memory note. Synchronous on purpose — the loop runs it off the event loop via
    ``asyncio.to_thread``."""

    def __init__(self, claude_bin: str = "claude", cwd: Optional[str] = None,
                 system_prompt: str = VOICE_SYSTEM_PROMPT, timeout: float = 120.0,
                 memory_prompt: Optional[str] = None,
                 memory_tools: Sequence[str] = MEMORY_TOOLS) -> None:
        self._bin = claude_bin
        self._cwd = cwd
        self._system_prompt = system_prompt
        self._timeout = timeout
        self._memory_prompt = memory_prompt
        self._memory_tools = tuple(memory_tools)
        self._session_id: Optional[str] = None
        self._turns = 0  # turns in the current conversation, reset on end_session

    def _invoke(self, prompt: str, *, resume: bool, system: Optional[str] = None,
                allow_tools: Optional[Sequence[str]] = None) -> str:
        argv = [self._bin, "-p", prompt, "--output-format", "json"]
        if system:
            argv += ["--append-system-prompt", system]
        if resume and self._session_id:
            argv += ["--resume", self._session_id]
        if allow_tools:
            argv += ["--allowedTools", ",".join(allow_tools)]
        try:
            proc = subprocess.run(argv, cwd=self._cwd, capture_output=True,
                                  text=True, timeout=self._timeout)
        except FileNotFoundError as e:
            raise BrainError(f"brain command not found: {self._bin!r}") from e
        except subprocess.TimeoutExpired as e:
            raise BrainError(f"brain timed out after {self._timeout:.0f}s") from e
        if proc.returncode != 0:
            raise BrainError(proc.stderr.strip() or f"brain exited {proc.returncode}")
        try:
            data = json.loads(proc.stdout)
        except (ValueError, TypeError) as e:
            raise BrainError(f"brain returned unparseable output: {e}") from e
        sid = data.get("session_id")
        if sid:
            self._session_id = sid  # carry the conversation forward
        if data.get("is_error"):
            raise BrainError(str(data.get("result") or "brain reported an error"))
        return (data.get("result") or "").strip()

    def reply(self, transcript: str) -> str:
        out = self._invoke(transcript, resume=True, system=self._system_prompt)
        self._turns += 1
        return out

    def end_session(self) -> None:
        try:
            if self._memory_prompt and self._session_id and self._turns > 0:
                prompt = self._memory_prompt.replace(
                    "{date}", date.today().strftime("%Y_%m_%d"))
                log.info("saving voice session memory (%d turns)...", self._turns)
                self._invoke(prompt, resume=True, allow_tools=self._memory_tools)
        except BrainError as e:
            log.warning("memory save failed: %s", e)
        finally:
            self._session_id = None
            self._turns = 0


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
async def run_voice_loop(io: VoiceIO, brain: Brain, *, idle_timeout_s: float = 150.0,
                         max_turns: Optional[int] = None) -> None:
    """Drive the device voice loop through `brain`. After `idle_timeout_s` of
    silence the brain's `end_session` runs (persist memory / reset the
    conversation); set <= 0 to wait forever and never auto-end. A brain failure
    is handled per-turn (error crab + spoken apology) so one bad turn doesn't
    kill the loop. `max_turns` (for tests) stops after that many utterances."""
    timeout = idle_timeout_s if idle_timeout_s and idle_timeout_s > 0 else None
    turn = 0
    while max_turns is None or turn < max_turns:
        await io.set_state("idle")
        text = await io.wait_for_utterance(timeout)
        if not text:
            # silence (or idle timeout): close out the conversation if one's open
            await asyncio.to_thread(brain.end_session)
            continue
        turn += 1
        log.info("heard: %r", text)
        await io.set_state("thinking")
        try:
            reply = await asyncio.to_thread(brain.reply, text)
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
            await run_voice_loop(McpVoiceIO(session), brain, idle_timeout_s=idle_timeout_s)


def main() -> None:
    parser = argparse.ArgumentParser(prog="voice_agent")
    parser.add_argument("--brain-cmd", default="claude",
                        help="brain executable (default: claude, headless -p mode)")
    parser.add_argument("--brain-cwd", default=None,
                        help="working dir for the brain — point at your agent's "
                             "vault/project dir so it inherits that context/persona")
    parser.add_argument("--brain-timeout", type=float, default=120.0,
                        help="seconds to wait for the brain per turn (default: 120)")
    parser.add_argument("--idle-timeout", type=float, default=150.0,
                        help="seconds of silence that ends a conversation and "
                             "triggers a memory save; <=0 to never auto-end")
    parser.add_argument("--memory-prompt", default=None,
                        help="when a conversation goes idle, this is sent to the "
                             "brain (with its session resumed + write tools) to save "
                             "memory. '{date}' expands to YYYY_MM_DD. Omit to disable.")
    parser.add_argument("--host", default="0.0.0.0", help="device-link bind address")
    parser.add_argument("--port", type=int, default=8765, help="device-link port")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
                        stream=sys.stderr)
    brain = ClaudeBrain(args.brain_cmd, args.brain_cwd, timeout=args.brain_timeout,
                        memory_prompt=args.memory_prompt)
    try:
        asyncio.run(_serve(brain, args.host, args.port, args.idle_timeout))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

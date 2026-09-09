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

The loop, per turn (the agent sets thinking/speaking; the device itself shows
"listening" while waiting for you and "idle" when the conversation ends, so a
follow-up window doesn't look asleep):

    wait_for_utterance  ->  thinking  ->  brain.reply(text)  ->  speaking  ->  speak

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
import os
import re
import sys
import time

# Run as a script (`python tools/voice_agent.py`), Python puts *tools/* on
# sys.path — not the bridge dir — so `clawlexa_bridge` wouldn't import. The
# bridge is spawned as a subprocess (`-m clawlexa_bridge`, which resolves via
# its cwd), but the presence code imports the package directly, in-process.
_BRIDGE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _BRIDGE_DIR not in sys.path:
    sys.path.insert(0, _BRIDGE_DIR)
from abc import ABC, abstractmethod
from datetime import date
from typing import Callable, Optional, Sequence

log = logging.getLogger("clawlexa.voice")

# Marker the brain appends when it judges the conversation over; stripped before
# playback and used to end the conversation (device re-arms its wake word).
END_SENTINEL = "<end>"

# Spoken replies, not chat: keep the brain terse and TTS-friendly.
VOICE_SYSTEM_PROMPT = (
    "You are a voice assistant speaking through a small speaker. Reply in one or "
    "two short, natural spoken sentences. Do not use markdown, lists, code blocks, "
    "URLs, or emoji — your reply is read aloud by text-to-speech. "
    "Keep every reply fast: do not save notes, write or edit files, or run git "
    "while replying — a session summary is saved for you automatically afterward. "
    "Just talk. "
    "This reply is the whole turn: you cannot go away, look something up, and "
    "come back. Nothing you promise for later will ever happen, and the device "
    "falls silent and goes to sleep while the user waits for it. So never say "
    "you'll check, pull something up, dig into it, or get back to them. Answer "
    "now from what you already know, or say plainly that you don't have it to "
    "hand and ask them to look it up with you another way. "
    "When the conversation is naturally finished — the user says goodbye, signs "
    "off, or otherwise ends it — give your short farewell and then append the "
    f"marker {END_SENTINEL} on its own at the very end. The marker is removed "
    "before playback; it tells the device the chat is over so it can stop "
    "listening. Only add it when you're genuinely wrapping up."
)
# Where a Home Assistant long-lived token lives by default (never logged).
DEFAULT_TOKEN_PATH = "~/.config/ha-token"

# Claude Code's own tools. Naming ANY allowed tool switches the CLI to an
# allowlist, which silently disables everything unnamed — so these have to be
# repeated alongside the MCP entries or the brain quietly loses Read, WebFetch
# and the rest. (The same trap is called out in iPinch's Slack handler.)
BUILTIN_TOOLS = (
    "WebFetch", "WebSearch",
    "Read", "Write", "Edit", "MultiEdit",
    "Bash", "Glob", "Grep",
    "Task", "TodoRead", "TodoWrite",
    "NotebookRead", "NotebookEdit",
)


def load_mcp_servers(path: "Optional[str]") -> "Optional[dict]":
    """Read a Claude-style MCP config (`{"mcpServers": {...}}`) from `path`.

    Returns None when no path is given. Raises with a clear message otherwise —
    a typo'd path should stop the driver at startup, not silently produce a crab
    with no tools that nobody notices until it can't answer a question.
    """
    if not path:
        return None
    import json
    import os

    full = os.path.expanduser(path)
    with open(full, "r") as f:
        config = json.load(f)
    servers = config.get("mcpServers") if isinstance(config, dict) else None
    if not isinstance(servers, dict) or not servers:
        raise ValueError(f"no 'mcpServers' object in {full}")
    return servers


def parse_quiet_hours(spec: str) -> tuple[int, int]:
    """Parse a '22-8' quiet-hours window into (start, end) local hours. Equal
    values mean no quiet hours at all."""
    try:
        start, end = (int(p) for p in spec.split("-", 1))
    except ValueError:
        raise ValueError(f"quiet hours must look like '22-8', got {spec!r}") from None
    if not (0 <= start <= 23 and 0 <= end <= 23):
        raise ValueError(f"quiet hours must be 0-23, got {spec!r}")
    return start, end


def parse_greetings(specs) -> "Optional[dict]":
    """Parse repeated `--greeting WHEN:TEXT` args into a greeting table.

    WHEN is morning|afternoon|evening; repeat the flag for several lines and they
    rotate. A time of day you don't mention keeps its built-in line, so you can
    override just the mornings. Returns None when nothing was passed, meaning
    "use the defaults" — the repo's lines stay generic (SPEC §2) and a
    deployment with a persona supplies its own.
    """
    if not specs:
        return None
    from clawlexa_bridge.presence import DEFAULT_GREETINGS

    custom: dict = {}
    for spec in specs:
        when, sep, line = spec.partition(":")
        when = when.strip().lower()
        if not sep or not line.strip():
            raise ValueError(f"--greeting wants 'when:text', got {spec!r}")
        if when not in DEFAULT_GREETINGS:
            raise ValueError(f"unknown greeting time {when!r}; expected one of "
                             f"{', '.join(sorted(DEFAULT_GREETINGS))}")
        custom.setdefault(when, []).append(line.strip())
    table = {k: tuple(v) for k, v in DEFAULT_GREETINGS.items()}
    table.update({k: tuple(v) for k, v in custom.items()})
    return table


# Spoken when a turn is taking long enough that silence would read as a hang.
# Rotates so a run of slow turns doesn't sound like a stuck record.
HOLDING_LINES = (
    "Let me have a look.",
    "One moment, checking that.",
    "Hang on, looking that up.",
)
DEFAULT_HOLDING_AFTER_S = 4.0

BRAIN_ERROR_REPLY = "Sorry, I hit a problem thinking about that."
EMPTY_BRAIN_REPLY = "I didn't catch that — could you say it again?"

# Sent once when a session first opens (pre-warm), before anyone speaks. It warms
# the model (so turn one isn't a cold generation), has the agent load its persona
# and memory (so it's in character from the first word), and tells it it's now
# speaking through clawlexa. The reply is discarded. Deployment-specific wording
# (e.g. "read soul.md", the user's name) is passed via --warm-prompt.
WARM_PROMPT = (
    "You're starting a voice session through clawlexa — your hardware body: a small "
    "device with a microphone and speaker that a person talks to out loud. Load your "
    "usual persona, context, and memory now so you're fully yourself and ready. This "
    "is a spoken session, read aloud by text-to-speech, so keep replies short and "
    "natural with no markdown. Reply with only: ready"
)

# Farewell phrases: if the user's utterance clearly signs off, end the
# conversation even if the brain forgot the sentinel (belt-and-suspenders).
_FAREWELL_RE = re.compile(
    r"\b(bye|goodbye|good ?night|see (you|ya)|talk to you later|"
    r"speak to you later|catch you later|that'?s all( for now)?|"
    r"we'?re (all )?done|i'?m done( here)?)\b",
    re.IGNORECASE,
)


def strip_end_sentinel(reply: str) -> tuple[str, bool]:
    """Return (reply without the END_SENTINEL, whether it was present)."""
    if END_SENTINEL in reply:
        return reply.replace(END_SENTINEL, "").strip(), True
    return reply, False


def is_farewell(text: str) -> bool:
    """True if `text` reads like the user signing off."""
    return bool(_FAREWELL_RE.search(text))


# --- token / cost tracking --------------------------------------------------
class CostMeter:
    """Running token + cost tally for the voice path, so we can watch spend and
    compare it against the Slack path. `total_cost_usd` comes straight from the
    Agent SDK's per-turn ResultMessage (the Claude Code CLI computes it,
    cache-aware); `usage` carries the token breakdown. Pure / host-tested; process
    lifetime totals (survives warm-session resets)."""

    # Fields the CLI reports under ResultMessage.usage.
    TOKENS = ("input_tokens", "output_tokens",
              "cache_read_input_tokens", "cache_creation_input_tokens")

    # Haiku 4.5 list rates, $ per token: input, output, cache read (0.1x input),
    # 5-minute cache write (1.25x input). Only used to sanity-check the CLI's own
    # figure — see `_reconcile`. Update if --brain-model changes tier.
    RATES = {"input_tokens": 1.0e-6, "output_tokens": 5.0e-6,
             "cache_read_input_tokens": 0.1e-6, "cache_creation_input_tokens": 1.25e-6}

    def __init__(self) -> None:
        self.turns = 0
        self.cost_usd = 0.0
        self.tokens = {k: 0 for k in self.TOKENS}

    def record(self, usage: Optional[dict], cost_usd: Optional[float]) -> str:
        """Fold in one turn's usage/cost; return a one-line summary of THIS turn."""
        u = usage or {}
        this = {k: int(u.get(k, 0) or 0) for k in self.TOKENS}
        cost = float(cost_usd or 0.0)
        self.turns += 1
        self.cost_usd += cost
        for k in self.TOKENS:
            self.tokens[k] += this[k]
        return self._fmt(this, cost) + self._reconcile(u, this, cost)

    @classmethod
    def _reconcile(cls, usage: dict, tokens: dict, cost: float) -> str:
        """Explain the turn's cost when the headline `usage` doesn't account for it.

        `total_cost_usd` covers every API iteration the agent made this turn,
        while `usage` is the shape the CLI reports alongside it — so a turn that
        looped (reading files, re-priming) costs far more than these token counts
        imply, with nothing in the log to say so. We saw ~26x. `usage.iterations`
        is where the truth is, so surface its length and the implied-vs-charged
        gap; both are free to read.
        """
        iters = usage.get("iterations")
        n = len(iters) if isinstance(iters, list) else None
        implied = sum(tokens[k] * cls.RATES[k] for k in cls.TOKENS)
        bits = []
        bits.append("iters=%s" % (n if n is not None else "absent"))
        # A ratio near 1.0 means the logged tokens explain the bill.
        unexplained = implied > 0 and cost > 0 and cost / implied >= 1.5
        if unexplained:
            bits.append("UNACCOUNTED %.1fx (logged tokens imply $%.4f)"
                        % (cost / implied, implied))
            # Dump what the SDK actually handed us. The headline fields don't add
            # up, so the answer is in a field we aren't reading — a second cache
            # write, a per-iteration breakdown, a service tier. Only on turns
            # that don't reconcile, so it can't spam a healthy log.
            try:
                import json as _json
                raw = _json.dumps(usage, default=str, sort_keys=True)
            except Exception:
                raw = repr(usage)
            log.info("unreconciled turn, raw usage: %s",
                     raw[:900] + ("…" if len(raw) > 900 else ""))
        return ("  [%s]" % " ".join(bits)) if bits else ""

    def totals_line(self) -> str:
        return "session total: %d turns, %s" % (
            self.turns, self._fmt(self.tokens, self.cost_usd))

    @staticmethod
    def _fmt(tok: dict, cost: float) -> str:
        return ("in=%d out=%d cache_r=%d cache_w=%d $%.4f" % (
            tok["input_tokens"], tok["output_tokens"],
            tok["cache_read_input_tokens"], tok["cache_creation_input_tokens"], cost))


# Fields on the SDK's ResultMessage worth seeing while we chase the cost gap. The
# turn's own `usage` says one cheap cached iteration, yet the bill is ~16x that,
# and a direct `claude -p` call reconciles to the cent — so the discrepancy is
# somewhere on the session path. `model_usage` names the model that actually
# served the turn, which is the next thing to rule in or out.
RESULT_FIELDS = ("model_usage", "modelUsage", "model", "num_turns", "subtype",
                 "duration_ms", "duration_api_ms", "is_error", "session_id",
                 "permission_denials", "service_tier", "speed", "fast_mode_state")


def describe_result(msg) -> str:
    """One line of whatever the ResultMessage will tell us, skipping the fields
    we already log and the reply text. Deliberately broad while the cost gap is
    unexplained — it is free (the object is already in hand) and can be trimmed
    once the cause is known."""
    import json as _json
    seen = {}
    for f in RESULT_FIELDS:
        v = getattr(msg, f, None)
        if v not in (None, [], {}, ""):
            seen[f] = v
    # Anything else public we haven't thought to name, so a field that only
    # exists on some SDK versions can't hide from us.
    for f in sorted(vars(msg)) if hasattr(msg, "__dict__") else []:
        if (not f.startswith("_") and f not in seen
                and f not in ("usage", "total_cost_usd", "result")):
            v = getattr(msg, f, None)
            if v not in (None, [], {}, ""):
                seen.setdefault(f, v)
    try:
        out = _json.dumps(seen, default=str, sort_keys=True)
    except Exception:
        out = repr(seen)
    return out[:1200] + ("…" if len(out) > 1200 else "")


def append_cost_row(path: str, label: str, usage: Optional[dict],
                    cost_usd: Optional[float]) -> None:
    """Append one turn's usage/cost to a CSV (created with a header) for later
    analysis / voice-vs-Slack comparison. Best-effort — never breaks a reply."""
    import csv
    import os
    from datetime import datetime, timezone
    u = usage or {}
    new = not os.path.exists(path)
    try:
        with open(path, "a", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["ts", "label", "input", "output",
                            "cache_read", "cache_write", "cost_usd"])
            w.writerow([datetime.now(timezone.utc).isoformat(), label,
                        int(u.get("input_tokens", 0) or 0),
                        int(u.get("output_tokens", 0) or 0),
                        int(u.get("cache_read_input_tokens", 0) or 0),
                        int(u.get("cache_creation_input_tokens", 0) or 0),
                        float(cost_usd or 0.0)])
    except OSError as e:
        log.warning("cost-log write failed (%s): %s", path, e)


class BrainError(RuntimeError):
    """The brain failed to produce a reply (couldn't start, errored, timed out)."""


# --- the brain: transcript in, spoken reply out -----------------------------
class Brain(ABC):
    @abstractmethod
    async def reply(self, transcript: str) -> str:
        """Answer one utterance (and remember it for the rest of the conversation)."""

    async def warm(self) -> None:
        """Open/prepare the session ahead of the first utterance so turn one isn't
        cold. Default: nothing (a brain with no warm-up cost)."""
        return None

    async def end_session(self) -> None:
        """Conversation went idle — persist memory and/or tear down. Default: nothing."""
        return None


class ClaudeSessionBrain(Brain):
    """A brain backed by a **warm** Claude Agent SDK session. The session is
    opened by ``warm`` (called at startup, so turn one isn't cold) and kept alive
    across turns *and across conversations* — a goodbye ends the device turn but
    not this session, so it stays warm and remembers. It's closed only by
    ``end_session`` on the long idle reset, which first saves a memory note.
    Running it with ``cwd`` set to your agent's vault gives it that agent's
    CLAUDE.md / persona / memory. A lock serialises the client so a background
    idle-save can't collide with a concurrent reply.

    `client_factory` is injectable for tests; by default it builds a real
    ClaudeSDKClient (imported lazily so the dependency is optional)."""

    def __init__(self, cwd: Optional[str] = None, cli_path: Optional[str] = None,
                 system_prompt: str = VOICE_SYSTEM_PROMPT, timeout: float = 120.0,
                 memory_prompt: Optional[str] = None,
                 warm_prompt: Optional[str] = WARM_PROMPT,
                 cost_log: Optional[str] = None,
                 model: Optional[str] = None,
                 effort: Optional[str] = None,
                 permission_mode: str = "acceptEdits",
                 setting_sources: Sequence[str] = ("project", "user"),
                 mcp_servers: Optional[dict] = None,
                 allowed_tools: Optional[Sequence[str]] = None,
                 max_budget_usd: Optional[float] = None,
                 client_factory: Optional[Callable[[], object]] = None) -> None:
        self._cwd = cwd
        self._cli_path = cli_path
        self._system_prompt = system_prompt
        self._timeout = timeout
        self._memory_prompt = memory_prompt
        self._warm_prompt = warm_prompt
        self._cost_log = cost_log
        self._model = model      # None -> the CLI's default model
        self._effort = effort    # None -> default; must stay unset on Haiku (errors)
        self._cost = CostMeter()  # process-lifetime token/cost tally
        self._permission_mode = permission_mode
        self._setting_sources = tuple(setting_sources)
        self._mcp_servers = mcp_servers or None
        self._allowed_tools = tuple(allowed_tools) if allowed_tools else None
        self._max_budget_usd = max_budget_usd
        self._client_factory = client_factory or self._default_factory
        self._client = None
        self._turns = 0  # turns this session, reset when the session closes
        self._lock = asyncio.Lock()  # serialise client use (reply vs bg idle-save)

    def _client_options(self) -> dict:
        """The ClaudeAgentOptions kwargs. Pure (no SDK import) so it's host-testable.
        `model`/`effort` are only included when set — Haiku errors on `effort`, so
        it's left out by default."""
        kwargs = dict(
            cwd=self._cwd,
            cli_path=self._cli_path,
            system_prompt={"type": "preset", "preset": "claude_code",
                           "append": self._system_prompt},
            setting_sources=list(self._setting_sources),
            permission_mode=self._permission_mode,
        )
        if self._model:
            kwargs["model"] = self._model     # e.g. claude-haiku-4-5 for a cheap voice brain
        if self._effort:
            kwargs["effort"] = self._effort   # only on models that support it (not Haiku)
        if self._mcp_servers:
            kwargs["mcp_servers"] = self._mcp_servers
        if self._allowed_tools:
            kwargs["allowed_tools"] = list(self._allowed_tools)
        if self._max_budget_usd:
            # A seatbelt, not a plan: a tool turn that loops should cost a
            # bounded amount rather than whatever it takes to notice.
            kwargs["max_budget_usd"] = self._max_budget_usd
        return kwargs

    def _default_factory(self):
        try:
            from claude_agent_sdk import ClaudeAgentOptions, ClaudeSDKClient
        except ImportError as e:  # optional dep — only needed for this brain
            raise BrainError("claude-agent-sdk is not installed "
                             "(pip install claude-agent-sdk)") from e
        return ClaudeSDKClient(options=ClaudeAgentOptions(**self._client_options()))

    async def _ensure(self) -> bool:
        """Open the session if needed. Returns True if this call connected a fresh
        one (so the caller can prime it), False if it was already up."""
        if self._client is None:
            client = self._client_factory()  # may raise BrainError (missing dep)
            await client.connect()
            self._client = client
            return True
        return False

    async def _drain(self, label: str = "turn") -> str:
        """Collect the assistant's spoken text from one response; raise BrainError
        if the model reports an error (auth/billing/etc.). The trailing
        ResultMessage carries this turn's token usage + cost, which we log and
        tally (`label` distinguishes reply / warm / memory turns)."""
        parts = []
        async for msg in self._client.receive_response():
            tn = type(msg).__name__
            if tn == "AssistantMessage":
                err = getattr(msg, "error", None)
                if err:
                    raise BrainError(f"brain error: {err}")
                for block in getattr(msg, "content", None) or []:
                    if type(block).__name__ == "TextBlock":
                        parts.append(getattr(block, "text", ""))
            elif tn == "ResultMessage":
                usage = getattr(msg, "usage", None)
                cost = getattr(msg, "total_cost_usd", None)
                log.info("cost[%s]: %s | %s", label,
                         self._cost.record(usage, cost), self._cost.totals_line())
                log.info("result[%s]: %s", label, describe_result(msg))
                if self._cost_log:
                    append_cost_row(self._cost_log, label, usage, cost)
        return "".join(parts).strip()

    async def warm(self) -> None:
        """Pre-open the session so the first turn skips cold-start, and prime it
        (warm the model + load persona + note the clawlexa context). Failure is
        non-fatal — the first reply will retry and surface any real error."""
        async with self._lock:
            try:
                fresh = await self._ensure()
            except BrainError as e:
                log.warning("pre-warm failed (first turn will retry): %s", e)
                return
            if fresh and self._warm_prompt:  # prime only a newly-opened session
                try:
                    await asyncio.wait_for(self._client.query(self._warm_prompt), self._timeout)
                    ready = await asyncio.wait_for(self._drain("warm"), self._timeout)
                    log.info("session primed (%r)", ready[:40])
                except Exception as e:  # connection is up; don't block on a prime miss
                    log.warning("pre-warm priming failed (continuing): %s", e)

    async def reply(self, transcript: str) -> str:
        async with self._lock:
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
        async with self._lock:
            try:
                if self._client is not None and self._memory_prompt and self._turns > 0:
                    prompt = self._memory_prompt.replace(
                        "{date}", date.today().strftime("%Y_%m_%d"))
                    log.info("saving voice session memory (%d turns)...", self._turns)
                    await asyncio.wait_for(self._client.query(prompt), self._timeout)
                    await asyncio.wait_for(self._drain("memory"), self._timeout)
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
    """The bridge tools the loop needs, as an interface — an MCP-backed impl
    drives the real device, a fake drives the tests."""

    @abstractmethod
    async def wait_for_utterance(self, timeout_s: Optional[float] = None) -> str: ...
    @abstractmethod
    async def set_state(self, state: str) -> None: ...
    @abstractmethod
    async def speak(self, text: str) -> None: ...
    @abstractmethod
    async def show(self, text: str) -> None: ...
    @abstractmethod
    async def end_conversation(self) -> None: ...
    @abstractmethod
    async def listen(self) -> None: ...


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

    async def end_conversation(self) -> None:
        await self._session.call_tool("end_conversation", {})

    async def listen(self) -> None:
        await self._session.call_tool("listen", {})


# --- ambient greeting (SPEC §7a) --------------------------------------------
class Activity:
    """When the device was last busy with a conversation, so an ambient greeting
    never talks over one. The voice driver can't see the bridge's conversation
    window directly, so it tracks its own turns and treats the follow-up window
    as still-busy."""

    def __init__(self, window_s: float = 20.0,
                 now: Callable[[], float] = time.monotonic) -> None:
        self._window_s = window_s
        self._now = now
        self._last = None  # type: Optional[float]

    def touch(self) -> None:
        self._last = self._now()

    def busy(self) -> bool:
        return self._last is not None and (self._now() - self._last) < self._window_s


async def greet_on_arrival(io: VoiceIO, source, policy, activity: Activity, *,
                           greetings=None, max_greetings: Optional[int] = None) -> None:
    """Watch a presence source; greet the user when they arrive.

    The greeting is deliberately *not* a brain turn: a canned line plays straight
    away and a listening window opens after it, so walking past the study costs
    nothing and there's no cold-start pause between the door and the hello. If
    the user answers, the main voice loop picks the utterance up and the brain
    takes over from there (SPEC §7a).

    `max_greetings` (for tests) returns after that many greetings.
    """
    greeted = 0
    async for reading in source.readings():
        # Readings carry how long the sensor has held this state (nonzero only
        # for the startup baseline), so a restart resumes a real absence instead
        # of restarting the away clock. Plain bools are still accepted so a test
        # fake can stay a list of True/False.
        occupied, steady_for_s = (reading if isinstance(reading, tuple)
                                  else (reading, 0.0))
        if not policy.update(occupied, busy=activity.busy(),
                             steady_for_s=steady_for_s):
            # Say why. A suppressed arrival is otherwise indistinguishable from
            # a sensor that never fired, and telling those apart after the fact
            # means reconstructing the timeline from Home Assistant's history.
            log.info("presence: %s", policy.reason)
            continue
        line = policy.greeting(greetings)
        log.info("presence: arrival -> greeting %r", line)
        try:
            await io.set_state("speaking")
            await io.speak(line)
            await io.listen()  # open a window so they can just answer
            activity.touch()
        except Exception as exc:  # device unplugged, bridge restarting, ...
            log.warning("presence greeting failed (%s) — skipping it", exc)
            # We set "speaking" above, and in this path nothing else ever clears
            # it: the device only returns to idle when a conversation *ends*, and
            # if speak/listen failed no conversation ever began. Without this the
            # crab sits looking like it's talking until the next wake. Best
            # effort — if the device is truly gone this fails too, and the
            # firmware's link-down handling takes over.
            try:
                await io.set_state("idle")
            except Exception:
                pass
        greeted += 1
        if max_greetings is not None and greeted >= max_greetings:
            return


# --- the loop ---------------------------------------------------------------
async def reply_with_holding_line(io: VoiceIO, brain: Brain, text: str, *,
                                  holding_after_s: float = DEFAULT_HOLDING_AFTER_S,
                                  holding_index: int = 0) -> str:
    """Ask the brain for a reply; if it takes long enough that the silence would
    read as a hang, say so and keep waiting.

    Tool-using turns are slow — reading a doc or querying the house takes seconds,
    not milliseconds — and dead air is indistinguishable from a crash. Worse, the
    device's follow-up window can expire mid-look-up and put it to sleep on a
    waiting user, which is exactly how last night's "let me pull that up" bug felt.

    The line comes from the driver, not the brain: the brain is busy, and asking
    it to announce itself first would need a second round trip. Same reasoning
    that made the ambient greeting canned.
    """
    task = asyncio.ensure_future(brain.reply(text))
    done, _ = await asyncio.wait({task}, timeout=holding_after_s)
    if not done:
        line = HOLDING_LINES[holding_index % len(HOLDING_LINES)]
        log.info("slow turn -> holding line %r", line)
        try:
            await io.speak(line)
        except Exception as exc:  # never let the filler kill the real reply
            log.warning("holding line failed (%s)", exc)
    return await task


async def run_voice_loop(io: VoiceIO, brain: Brain, *, idle_timeout_s: float = 1800.0,
                         max_turns: Optional[int] = None,
                         activity: Optional[Activity] = None,
                         holding_after_s: float = DEFAULT_HOLDING_AFTER_S) -> None:
    """Drive the device voice loop through `brain`. The brain is pre-warmed at
    startup so turn one isn't a cold start, and its session stays warm across turns
    *and across conversations* — a goodbye ends the device turn, not the session.
    After `idle_timeout_s` of silence the session is closed in the background:
    `end_session` saves a memory checkpoint and drops the warm session (it is NOT
    re-primed — that re-writes the vault context to cache and burns credits on an
    unused session; the next wake reconnects lazily). Set `idle_timeout_s` <= 0 to
    never idle-reset. A brain failure is
    handled per-turn (error crab + spoken apology) so one bad turn doesn't kill the
    loop. `max_turns` (for tests) stops after that many utterances; any background
    resets are awaited before returning."""
    timeout = idle_timeout_s if idle_timeout_s and idle_timeout_s > 0 else None
    turn = 0
    bg: set = set()  # background tasks (idle memory-save + re-warm); awaited on exit

    def spawn(coro) -> None:
        t = asyncio.create_task(coro)
        bg.add(t)
        t.add_done_callback(bg.discard)
    # Idle/listening are firmware-driven at the conversation boundaries: the
    # device shows "listening" on wake and after each reply (the follow-up window
    # is open), and "idle" when the bridge ends the conversation. So the agent
    # only sets the initial idle and, per turn, thinking/speaking/error — it must
    # NOT force idle after speaking, or the crab looks asleep while the device is
    # still listening for a follow-up (SPEC §7).
    await io.set_state("idle")
    await brain.warm()  # pre-open the session so the first turn isn't a cold start
    while max_turns is None or turn < max_turns:
        text = await io.wait_for_utterance(timeout)
        if not text:
            # Long idle: checkpoint memory + close the session in the background
            # (so a wake right at the timeout isn't blocked; the brain serialises
            # the save against the next reply). We deliberately do NOT re-prime
            # here — priming re-writes the whole vault context to cache (~$0.23 a
            # pop), and re-priming on every idle tick burns credits on a session
            # nobody may use. The next real wake reconnects lazily instead.
            spawn(brain.end_session())
            continue
        turn += 1
        log.info("heard: %r", text)
        if activity is not None:  # a conversation is live: hold off any greeting
            activity.touch()
        await io.set_state("thinking")
        try:
            reply = await reply_with_holding_line(
                io, brain, text, holding_after_s=holding_after_s,
                holding_index=turn - 1)
        except BrainError as e:
            log.warning("brain error: %s", e)
            await io.set_state("error")
            await io.speak(BRAIN_ERROR_REPLY)
            continue
        if not reply:
            reply = EMPTY_BRAIN_REPLY
        # Natural conversation end: the brain marks a goodbye with END_SENTINEL, or
        # the user's own words clearly sign off. Strip the marker from what we
        # speak, then (after the farewell plays) end the conversation so the device
        # re-arms now instead of sitting through the follow-up silence window.
        reply, wants_end = strip_end_sentinel(reply)
        over = wants_end or is_farewell(text)
        if not reply:  # sentinel was the whole reply — still say something
            reply = EMPTY_BRAIN_REPLY
        log.info("reply: %r%s", reply, "  [end]" if over else "")
        await io.set_state("speaking")
        await io.speak(reply)
        if activity is not None:  # the follow-up window is open — still busy
            activity.touch()
        if over:
            await io.end_conversation()
    if bg:  # let any in-flight idle memory-save finish
        await asyncio.gather(*bg, return_exceptions=True)


def build_presence(args) -> tuple:
    """Build (source, policy) from the CLI args, or (None, None) if the ambient
    greeting isn't configured. Failing to read the token is fatal *here*, at
    startup, rather than silently never greeting."""
    if not args.ha_url or not args.ha_entity:
        return None, None
    from clawlexa_bridge.ha import HomeAssistantPresence, read_token
    from clawlexa_bridge.presence import GreetingPolicy

    token = read_token(args.ha_token_file)
    source = HomeAssistantPresence(args.ha_url, token, args.ha_entity)
    quiet_start, quiet_end = args.quiet_hours
    policy = GreetingPolicy(away_s=args.away_minutes * 60,
                            min_gap_s=args.away_minutes * 60,
                            quiet_start_h=quiet_start, quiet_end_h=quiet_end)
    log.info("ambient greeting: %s, away>=%d min, quiet %02d:00-%02d:00",
             args.ha_entity, args.away_minutes, quiet_start, quiet_end)
    return source, policy


async def _serve(brain: Brain, host: str, port: int, idle_timeout_s: float,
                 source=None, policy=None, greetings=None,
                 holding_after_s: float = DEFAULT_HOLDING_AFTER_S) -> None:
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
            io = McpVoiceIO(session)
            activity = Activity()
            tasks = [asyncio.create_task(
                run_voice_loop(io, brain, idle_timeout_s=idle_timeout_s,
                               activity=activity,
                               holding_after_s=holding_after_s))]
            if source is not None:
                log.info("watching for arrivals in the room")
                tasks.append(asyncio.create_task(
                    greet_on_arrival(io, source, policy, activity,
                                     greetings=greetings)))
            try:
                # Either task ending (or failing) ends the session; the greeting
                # watcher reconnects internally, so it normally runs forever.
                done, pending = await asyncio.wait(
                    tasks, return_when=asyncio.FIRST_COMPLETED)
                for t in pending:
                    t.cancel()
                for t in done:
                    t.result()  # re-raise whatever stopped us
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
    parser.add_argument("--warm-prompt", default=WARM_PROMPT,
                        help="sent once when a session opens (pre-warm), before anyone "
                             "speaks: warms the model and has the agent load its persona "
                             "so turn one is fast and in character. Set '' to disable.")
    parser.add_argument("--cost-log", default=None,
                        help="append per-turn token usage + cost to this CSV file "
                             "(for watching voice spend / comparing against Slack). "
                             "Per-turn cost is always logged to stderr regardless.")
    parser.add_argument("--brain-model", default=None,
                        help="model for the voice brain (e.g. claude-haiku-4-5 or "
                             "claude-sonnet-4-6). Default: the Claude CLI's own default. "
                             "Voice is light work, so a cheaper model cuts cost a lot.")
    parser.add_argument("--effort", default=None,
                        help="reasoning effort: low|medium|high|max. Lower = cheaper/faster. "
                             "Only on Opus 4.5+/Sonnet 4.6 — leave unset for Haiku (it errors).")
    # --- tools (SPEC §12 Phase 6d) ---
    parser.add_argument("--mcp-config", default=None, metavar="PATH",
                        help="Claude-style MCP config ({\"mcpServers\": {...}}) to give "
                             "the brain. Point it at the same file your other entry "
                             "points use so they can't drift apart. Without it the "
                             "brain has only Claude Code's built-in tools.")
    parser.add_argument("--allow-tool", action="append", default=None, metavar="NAME",
                        help="permit one tool, e.g. mcp__home-assistant__ha_get_state, "
                             "or mcp__<server> for a whole server. Repeatable. Naming "
                             "any tool switches the CLI to an allowlist; the built-ins "
                             "are re-added automatically so they aren't lost. Prefer "
                             "naming read-only tools: a misheard sentence should not be "
                             "able to act on the world.")
    parser.add_argument("--holding-after", type=float, default=DEFAULT_HOLDING_AFTER_S,
                        metavar="SECONDS",
                        help=f"say a holding line ('let me have a look') once a turn has "
                             f"taken this long, so a slow tool call isn't dead air "
                             f"(default: {DEFAULT_HOLDING_AFTER_S:.0f}). 0 disables it.")
    parser.add_argument("--max-budget-usd", type=float, default=None,
                        help="stop a turn once it has cost this much (a seatbelt for "
                             "tool turns that loop)")

    # --- ambient presence greeting (SPEC §7a) ---
    parser.add_argument("--ha-url", default=None,
                        help="Home Assistant base URL (e.g. http://homeassistant.local:8123). "
                             "Set this together with --ha-entity to enable the ambient "
                             "greeting; omit either and the device stays wake-word only.")
    parser.add_argument("--ha-entity", default=None,
                        help="presence entity to watch, e.g. binary_sensor.study_presence")
    parser.add_argument("--ha-token-file", default=DEFAULT_TOKEN_PATH,
                        help=f"file holding a Home Assistant long-lived access token "
                             f"(default: {DEFAULT_TOKEN_PATH})")
    parser.add_argument("--away-minutes", type=int, default=30,
                        help="how long the room must have been empty before returning to "
                             "it earns a greeting (default: 30). Also the minimum gap "
                             "between two greetings.")
    parser.add_argument("--greeting", action="append", default=None, metavar="WHEN:TEXT",
                        help="a greeting line, e.g. --greeting \"morning:Morning, Pim.\" "
                             "WHEN is morning|afternoon|evening. Repeat for several lines "
                             "(they rotate); a time of day you don't mention keeps its "
                             "built-in line. Use this to give the crab a persona without "
                             "putting one in the repo.")
    parser.add_argument("--quiet-hours", default="22-8", metavar="START-END",
                        help="local-hour window with no greetings (default: 22-8). "
                             "Use '0-0' to greet around the clock.")
    parser.add_argument("--host", default="0.0.0.0", help="device-link bind address")
    parser.add_argument("--port", type=int, default=8765, help="device-link port")
    args = parser.parse_args()
    args.quiet_hours = parse_quiet_hours(args.quiet_hours)
    greetings = parse_greetings(args.greeting)
    mcp_servers = load_mcp_servers(args.mcp_config)
    # Only switch the CLI into allowlist mode when there is something to allow;
    # otherwise leave its defaults alone.
    allowed_tools = None
    if mcp_servers or args.allow_tool:
        allowed_tools = list(BUILTIN_TOOLS) + list(args.allow_tool or [])

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
                        stream=sys.stderr)
    if mcp_servers:
        log.info("MCP servers: %s", ", ".join(sorted(mcp_servers)))
        log.info("tools allowed beyond the built-ins: %s",
                 ", ".join(args.allow_tool or []) or "(none — read-only session)")
    brain = ClaudeSessionBrain(cwd=args.brain_cwd, cli_path=args.claude_cli,
                               timeout=args.brain_timeout, memory_prompt=args.memory_prompt,
                               warm_prompt=args.warm_prompt or None, cost_log=args.cost_log,
                               model=args.brain_model, effort=args.effort,
                               mcp_servers=mcp_servers, allowed_tools=allowed_tools,
                               max_budget_usd=args.max_budget_usd)
    source, policy = build_presence(args)
    try:
        asyncio.run(_serve(brain, args.host, args.port, args.idle_timeout,
                           source=source, policy=policy, greetings=greetings,
                           holding_after_s=args.holding_after))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

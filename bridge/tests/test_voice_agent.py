"""Unit tests for the standalone voice driver (tools/voice_agent.py).

No device, no MCP transport, no real brain: a FakeVoiceIO records the tool calls
the loop makes and feeds it canned utterances; a FakeBrain stands in for the
agent; ClaudeBrain's subprocess is mocked to check the continuity/memory wiring.
Follows the repo convention of driving the event loop with asyncio.run.
"""
import asyncio
import json
import types
from unittest import mock

from tools.voice_agent import (
    BRAIN_ERROR_REPLY,
    EMPTY_BRAIN_REPLY,
    Brain,
    BrainError,
    ClaudeBrain,
    VoiceIO,
    run_voice_loop,
)


class FakeVoiceIO(VoiceIO):
    """Records states/speech and replays a fixed list of utterances."""

    def __init__(self, utterances):
        self._utterances = list(utterances)
        self.states = []
        self.spoken = []
        self.shown = []

    async def wait_for_utterance(self, timeout_s=None):
        return self._utterances.pop(0) if self._utterances else ""

    async def set_state(self, state):
        self.states.append(state)

    async def speak(self, text):
        self.spoken.append(text)

    async def show(self, text):
        self.shown.append(text)


class FakeBrain(Brain):
    def __init__(self, fn=lambda t: f"reply to {t}"):
        self._fn = fn
        self.heard = []
        self.ended = 0

    def reply(self, transcript):
        self.heard.append(transcript)
        return self._fn(transcript)

    def end_session(self):
        self.ended += 1


# --- loop behaviour ---------------------------------------------------------
def test_happy_turn_drives_idle_thinking_speaking_and_speaks_reply():
    io = FakeVoiceIO(["what time is it"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: f"You asked: {t}"), max_turns=1))

    assert io.spoken == ["You asked: what time is it"]
    assert io.states == ["idle", "thinking", "speaking"]


def test_empty_utterance_ends_session_without_consuming_a_turn():
    io = FakeVoiceIO(["", "hello"])
    brain = FakeBrain(lambda t: "hi")
    asyncio.run(run_voice_loop(io, brain, max_turns=1))

    # the empty utterance triggers end_session and loops back to idle; only the
    # real "hello" becomes a turn.
    assert brain.ended == 1
    assert brain.heard == ["hello"]
    assert io.spoken == ["hi"]
    assert io.states == ["idle", "idle", "thinking", "speaking"]


def test_brain_error_shows_error_state_and_speaks_apology():
    def angry(_t):
        raise BrainError("boom")

    io = FakeVoiceIO(["break please"])
    asyncio.run(run_voice_loop(io, FakeBrain(angry), max_turns=1))

    assert io.spoken == [BRAIN_ERROR_REPLY]
    assert "error" in io.states


def test_empty_reply_falls_back_to_a_prompt_to_repeat():
    io = FakeVoiceIO(["mumble"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: ""), max_turns=1))

    assert io.spoken == [EMPTY_BRAIN_REPLY]


# --- ClaudeBrain: continuity + memory wiring --------------------------------
def _fake_proc(stdout):
    return types.SimpleNamespace(returncode=0, stdout=stdout, stderr="")


def test_claude_brain_resumes_session_across_turns():
    calls = []

    def fake_run(argv, **kw):
        calls.append(argv)
        # echo a session id so the brain carries the conversation forward
        return _fake_proc(json.dumps({"result": "ok", "session_id": "sess-123"}))

    brain = ClaudeBrain(claude_bin="claude", cwd="/vault")
    with mock.patch("tools.voice_agent.subprocess.run", side_effect=fake_run):
        assert brain.reply("first") == "ok"
        assert brain.reply("second") == "ok"

    # first turn has no --resume; second turn resumes the captured session id
    assert "--resume" not in calls[0]
    assert calls[1][calls[1].index("--resume") + 1] == "sess-123"
    # both turns request structured output so we can capture the session id
    assert "--output-format" in calls[0] and "json" in calls[0]


def test_claude_brain_end_session_saves_memory_then_resets():
    calls = []

    def fake_run(argv, **kw):
        calls.append(argv)
        return _fake_proc(json.dumps({"result": "ok", "session_id": "s1"}))

    brain = ClaudeBrain(claude_bin="claude", cwd="/vault",
                        memory_prompt="save to memory/{date}_voice.md then say ok")
    with mock.patch("tools.voice_agent.subprocess.run", side_effect=fake_run):
        brain.reply("remember the milk")
        brain.end_session()
        # session reset: a following reply must start fresh (no --resume)
        brain.reply("new convo")

    memory_call = calls[1]
    assert "--allowedTools" in memory_call            # memory turn may write
    assert "--resume" in memory_call                  # ...on the same session
    assert "{date}" not in " ".join(memory_call)      # the date placeholder expanded
    assert "--resume" not in calls[2]                 # session was reset after save


def test_claude_brain_end_session_without_memory_prompt_is_quiet():
    calls = []
    brain = ClaudeBrain(claude_bin="claude", cwd="/vault")  # no memory_prompt
    with mock.patch("tools.voice_agent.subprocess.run",
                    side_effect=lambda a, **k: calls.append(a) or _fake_proc(
                        json.dumps({"result": "ok", "session_id": "s1"}))):
        brain.reply("hi")
        brain.end_session()  # no memory prompt -> no extra invocation

    assert len(calls) == 1


def test_claude_brain_missing_binary_raises_brainerror():
    brain = ClaudeBrain(claude_bin="clawlexa-no-such-binary-xyz")
    try:
        brain.reply("hello")
    except BrainError as e:
        assert "not found" in str(e)
    else:
        raise AssertionError("expected BrainError for a missing brain binary")

"""Unit tests for the standalone voice driver (tools/voice_agent.py).

No device, no MCP transport, no real SDK/network: a FakeVoiceIO records the tool
calls the loop makes and feeds it canned utterances; a FakeBrain stands in for
the agent; a FakeClient (injected via client_factory) stands in for the warm
Claude Agent SDK session. Follows the repo convention of driving the event loop
with asyncio.run.
"""
import asyncio

from tools.voice_agent import (
    BRAIN_ERROR_REPLY,
    EMPTY_BRAIN_REPLY,
    Brain,
    BrainError,
    ClaudeSessionBrain,
    VoiceIO,
    run_voice_loop,
)


# --- fakes ------------------------------------------------------------------
class FakeVoiceIO(VoiceIO):
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

    async def reply(self, transcript):
        self.heard.append(transcript)
        r = self._fn(transcript)
        if isinstance(r, Exception):
            raise r
        return r

    async def end_session(self):
        self.ended += 1


# Duck types matched by ClaudeSessionBrain._drain via class name (no SDK needed).
class TextBlock:
    def __init__(self, text):
        self.text = text


class AssistantMessage:
    def __init__(self, content, error=None):
        self.content = content
        self.error = error


class FakeClient:
    """Stands in for a warm ClaudeSDKClient: each query() is paired with the next
    receive_response() yielding one canned message list."""

    def __init__(self, responses):
        self._responses = list(responses)
        self.connected = 0
        self.disconnected = 0
        self.queries = []

    async def connect(self):
        self.connected += 1

    async def disconnect(self):
        self.disconnected += 1

    async def query(self, prompt, session_id="default"):
        self.queries.append(prompt)

    async def receive_response(self):
        msgs = self._responses.pop(0) if self._responses else []
        for m in msgs:
            yield m


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

    assert brain.ended == 1
    assert brain.heard == ["hello"]
    assert io.spoken == ["hi"]
    assert io.states == ["idle", "idle", "thinking", "speaking"]


def test_brain_error_shows_error_state_and_speaks_apology():
    io = FakeVoiceIO(["break please"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: BrainError("boom")), max_turns=1))

    assert io.spoken == [BRAIN_ERROR_REPLY]
    assert "error" in io.states


def test_empty_reply_falls_back_to_a_prompt_to_repeat():
    io = FakeVoiceIO(["mumble"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: ""), max_turns=1))

    assert io.spoken == [EMPTY_BRAIN_REPLY]


# --- ClaudeSessionBrain: warm session + memory ------------------------------
def test_brain_keeps_one_warm_session_across_turns():
    fc = FakeClient([[AssistantMessage([TextBlock("hi")])],
                     [AssistantMessage([TextBlock("again")])]])
    brain = ClaudeSessionBrain(client_factory=lambda: fc)

    async def run():
        return await brain.reply("one"), await brain.reply("two")

    assert asyncio.run(run()) == ("hi", "again")
    assert fc.connected == 1          # opened once, reused across turns
    assert fc.queries == ["one", "two"]


def test_end_session_saves_memory_then_closes():
    fc = FakeClient([[AssistantMessage([TextBlock("ok")])],   # the reply turn
                     [AssistantMessage([TextBlock("ok")])]])   # the memory-save turn
    brain = ClaudeSessionBrain(client_factory=lambda: fc,
                               memory_prompt="save to memory/{date}_voice.md then say ok")

    async def run():
        await brain.reply("remember the milk")
        await brain.end_session()

    asyncio.run(run())
    assert fc.disconnected == 1
    # a memory query was sent, with {date} expanded
    assert any("memory/" in q and "{date}" not in q for q in fc.queries)


def test_end_session_without_memory_prompt_just_closes():
    fc = FakeClient([[AssistantMessage([TextBlock("ok")])]])
    brain = ClaudeSessionBrain(client_factory=lambda: fc)  # no memory_prompt

    async def run():
        await brain.reply("hi")
        await brain.end_session()

    asyncio.run(run())
    assert fc.queries == ["hi"]        # no extra memory turn
    assert fc.disconnected == 1


def test_reply_on_model_error_raises_and_drops_the_session():
    fc = FakeClient([[AssistantMessage([TextBlock("")], error="billing_error")]])
    brain = ClaudeSessionBrain(client_factory=lambda: fc)

    async def run():
        try:
            await brain.reply("hi")
            return None
        except BrainError as e:
            return str(e)

    msg = asyncio.run(run())
    assert msg and "billing_error" in msg
    assert fc.disconnected == 1        # closed so the next turn reconnects fresh


def test_factory_failure_surfaces_as_brainerror():
    def boom():
        raise BrainError("claude-agent-sdk is not installed")

    brain = ClaudeSessionBrain(client_factory=boom)

    async def run():
        try:
            await brain.reply("hi")
            return None
        except BrainError as e:
            return str(e)

    assert "not installed" in asyncio.run(run())

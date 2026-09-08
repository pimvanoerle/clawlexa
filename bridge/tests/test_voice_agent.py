"""Unit tests for the standalone voice driver (tools/voice_agent.py).

No device, no MCP transport, no real SDK/network: a FakeVoiceIO records the tool
calls the loop makes and feeds it canned utterances; a FakeBrain stands in for
the agent; a FakeClient (injected via client_factory) stands in for the warm
Claude Agent SDK session. Follows the repo convention of driving the event loop
with asyncio.run.
"""
import asyncio
import sys

import pytest

from clawlexa_bridge.presence import DEFAULT_GREETINGS, GreetingPolicy
from tools.voice_agent import (
    BRAIN_ERROR_REPLY,
    EMPTY_BRAIN_REPLY,
    Activity,
    Brain,
    BrainError,
    ClaudeSessionBrain,
    CostMeter,
    VoiceIO,
    greet_on_arrival,
    is_farewell,
    parse_greetings,
    parse_quiet_hours,
    run_voice_loop,
    strip_end_sentinel,
)


# --- fakes ------------------------------------------------------------------
class FakeVoiceIO(VoiceIO):
    def __init__(self, utterances):
        self._utterances = list(utterances)
        self.states = []
        self.spoken = []
        self.shown = []
        self.ended_conversations = 0
        self.listens = 0

    async def wait_for_utterance(self, timeout_s=None):
        return self._utterances.pop(0) if self._utterances else ""

    async def set_state(self, state):
        self.states.append(state)

    async def speak(self, text):
        self.spoken.append(text)

    async def show(self, text):
        self.shown.append(text)

    async def end_conversation(self):
        self.ended_conversations += 1

    async def listen(self):
        self.listens += 1


class FakeBrain(Brain):
    def __init__(self, fn=lambda t: f"reply to {t}"):
        self._fn = fn
        self.heard = []
        self.ended = 0
        self.warmed = 0

    async def reply(self, transcript):
        self.heard.append(transcript)
        r = self._fn(transcript)
        if isinstance(r, Exception):
            raise r
        return r

    async def warm(self):
        self.warmed += 1

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


# Duck type matched by _drain via class name — carries per-turn usage + cost.
class ResultMessage:
    def __init__(self, usage=None, total_cost_usd=None):
        self.usage = usage
        self.total_cost_usd = total_cost_usd


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
    # One initial idle, then per-turn thinking/speaking; the agent does NOT force
    # idle after speaking (the device shows listening during the follow-up window).
    assert io.states == ["idle", "thinking", "speaking"]


def test_empty_utterance_ends_session_without_consuming_a_turn():
    io = FakeVoiceIO(["", "hello"])
    brain = FakeBrain(lambda t: "hi")
    asyncio.run(run_voice_loop(io, brain, max_turns=1))

    assert brain.ended == 1
    assert brain.heard == ["hello"]
    assert io.spoken == ["hi"]
    # idle is set once at startup (not per iteration): the empty turn adds none.
    assert io.states == ["idle", "thinking", "speaking"]


def test_brain_error_shows_error_state_and_speaks_apology():
    io = FakeVoiceIO(["break please"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: BrainError("boom")), max_turns=1))

    assert io.spoken == [BRAIN_ERROR_REPLY]
    assert "error" in io.states


def test_empty_reply_falls_back_to_a_prompt_to_repeat():
    io = FakeVoiceIO(["mumble"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: ""), max_turns=1))

    assert io.spoken == [EMPTY_BRAIN_REPLY]


def test_loop_pre_warms_the_brain_before_the_first_turn():
    """The session is opened at startup so turn one isn't a cold start."""
    io = FakeVoiceIO(["hey there"])
    brain = FakeBrain(lambda t: "hello")
    asyncio.run(run_voice_loop(io, brain, max_turns=1))

    assert brain.warmed >= 1


def test_goodbye_keeps_the_brain_session_warm():
    """A goodbye ends the *device* conversation but does not close the brain
    session — it stays warm across conversations (only the idle reset closes it)."""
    io = FakeVoiceIO(["okay bye"])
    brain = FakeBrain(lambda t: "See ya!")
    asyncio.run(run_voice_loop(io, brain, max_turns=1))

    assert io.ended_conversations == 1  # device re-armed
    assert brain.ended == 0             # session NOT closed on goodbye


# --- natural conversation end (goodbye) -------------------------------------
def test_farewell_utterance_ends_the_conversation():
    """The user signing off ends the conversation even without the sentinel."""
    io = FakeVoiceIO(["okay thanks, bye"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: "See ya!"), max_turns=1))

    assert io.spoken == ["See ya!"]
    assert io.ended_conversations == 1


def test_sentinel_reply_is_stripped_and_ends_conversation():
    """The brain's <end> marker ends the conversation and never reaches TTS."""
    io = FakeVoiceIO(["what's the time"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: "It's noon. Talk later! <end>"),
                               max_turns=1))

    assert io.spoken == ["It's noon. Talk later!"]  # sentinel stripped
    assert io.ended_conversations == 1


def test_normal_turn_leaves_conversation_open():
    io = FakeVoiceIO(["how are you"])
    asyncio.run(run_voice_loop(io, FakeBrain(lambda t: "Doing great!"), max_turns=1))

    assert io.ended_conversations == 0  # no re-arm — the follow-up window stays open


def test_strip_end_sentinel():
    assert strip_end_sentinel("bye now <end>") == ("bye now", True)
    assert strip_end_sentinel("still chatting") == ("still chatting", False)


def test_is_farewell():
    assert is_farewell("okay, talk to you later")
    assert is_farewell("Bye!")
    assert is_farewell("that's all for now")
    assert not is_farewell("what time is the game")


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


# --- cost tracking ----------------------------------------------------------
def test_cost_meter_accumulates():
    m = CostMeter()
    line = m.record({"input_tokens": 10, "output_tokens": 5,
                     "cache_read_input_tokens": 0, "cache_creation_input_tokens": 2}, 0.01)
    assert "in=10 out=5 cache_r=0 cache_w=2 $0.0100" == line
    m.record({"input_tokens": 20, "output_tokens": 5}, 0.02)  # missing cache keys -> 0
    assert m.turns == 2
    assert m.tokens["input_tokens"] == 30 and m.tokens["output_tokens"] == 10
    assert abs(m.cost_usd - 0.03) < 1e-9
    assert "2 turns" in m.totals_line() and "$0.0300" in m.totals_line()


def test_cost_meter_handles_missing_usage_and_cost():
    m = CostMeter()
    m.record(None, None)  # a turn with no usage/cost -> zeros, no crash
    assert m.turns == 1 and m.cost_usd == 0.0


def test_reply_tallies_cost_from_the_result_message():
    fc = FakeClient([[AssistantMessage([TextBlock("hi")]),
                      ResultMessage(usage={"input_tokens": 100, "output_tokens": 20,
                                           "cache_read_input_tokens": 500,
                                           "cache_creation_input_tokens": 0},
                                    total_cost_usd=0.0123)]])
    brain = ClaudeSessionBrain(client_factory=lambda: fc)

    assert asyncio.run(brain.reply("hello")) == "hi"  # text still returned
    assert brain._cost.turns == 1
    assert abs(brain._cost.cost_usd - 0.0123) < 1e-9
    assert brain._cost.tokens["input_tokens"] == 100
    assert brain._cost.tokens["cache_read_input_tokens"] == 500


def test_client_options_includes_model_and_effort_when_set():
    b = ClaudeSessionBrain(model="claude-haiku-4-5", effort="low",
                           client_factory=lambda: None)
    opts = b._client_options()
    assert opts["model"] == "claude-haiku-4-5"
    assert opts["effort"] == "low"


def test_client_options_omits_model_and_effort_when_unset():
    b = ClaudeSessionBrain(client_factory=lambda: None)
    opts = b._client_options()
    assert "model" not in opts and "effort" not in opts  # Haiku errors on effort


def test_warm_primes_a_fresh_session():
    """Pre-warm opens the session and sends the priming prompt once."""
    fc = FakeClient([[AssistantMessage([TextBlock("ready")])]])
    brain = ClaudeSessionBrain(client_factory=lambda: fc, warm_prompt="PRIME NOW")

    asyncio.run(brain.warm())
    assert fc.connected == 1
    assert fc.queries == ["PRIME NOW"]  # model warmed + persona loaded before turn 1


def test_warm_without_prompt_only_connects():
    fc = FakeClient([])
    brain = ClaudeSessionBrain(client_factory=lambda: fc, warm_prompt=None)

    asyncio.run(brain.warm())
    assert fc.connected == 1
    assert fc.queries == []  # nothing sent when priming is disabled


def test_warm_does_not_reprime_an_open_session():
    fc = FakeClient([[AssistantMessage([TextBlock("ready")])]])
    brain = ClaudeSessionBrain(client_factory=lambda: fc, warm_prompt="PRIME")

    async def run():
        await brain.warm()
        await brain.warm()  # already open — must not connect or prime again

    asyncio.run(run())
    assert fc.connected == 1
    assert fc.queries == ["PRIME"]  # primed exactly once


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


# --- ambient presence greeting (SPEC §7a) ------------------------------------
class FakeSensor:
    """A presence source that replays a canned list of readings."""

    def __init__(self, readings):
        self._readings = list(readings)

    async def readings(self):
        for r in self._readings:
            yield r


class GreetClock:
    def __init__(self):
        self.t = 0.0
        self.hour = 10

    def __call__(self):
        return self.t

    def advance(self, dt):
        self.t += dt


def greet_setup(readings, away_s=60.0, window_s=20.0):
    clk = GreetClock()
    io = FakeVoiceIO([])
    policy = GreetingPolicy(away_s=away_s, min_gap_s=away_s, quiet_start_h=22,
                            quiet_end_h=8, now=clk, hour=lambda: clk.hour)
    activity = Activity(window_s=window_s, now=clk)
    # The sensor's readings are separated by a jump on the fake clock, so
    # "cleared, then came back 10 minutes later" costs no real time.
    class Timed(FakeSensor):
        async def readings(self):
            for r in self._readings:
                clk.advance(10 * 60)
                yield r
    return io, Timed(readings), policy, activity, clk


def test_arrival_speaks_then_opens_a_listening_window():
    """The whole point: a canned hello, then a window so the user can answer
    without a wake word — and no brain involved at all."""
    io, sensor, policy, activity, _ = greet_setup([True, False, True])
    asyncio.run(greet_on_arrival(io, sensor, policy, activity))
    assert len(io.spoken) == 1
    assert io.spoken[0] in ("Morning! I'm here if you need me.",
                            "Morning. Good to see you.",
                            "Hey, morning. Ready when you are.")
    assert io.listens == 1
    assert io.states == ["speaking"]


def test_no_greeting_while_a_conversation_is_live():
    # A busy window wide enough to still be open when the sensor fires, since
    # the fake clock jumps 10 minutes per reading.
    io, sensor, policy, activity, _ = greet_setup([True, False, True],
                                                  window_s=60 * 60)
    activity.touch()  # mid-conversation when the arrival lands
    asyncio.run(greet_on_arrival(io, sensor, policy, activity))
    assert io.spoken == [] and io.listens == 0


def test_greeting_survives_a_device_that_is_not_there():
    """Unplugged device / restarting bridge: log it and carry on, don't crash
    the voice driver. (The display tidy-up on this path is covered below.)"""
    io, sensor, policy, activity, _ = greet_setup([True, False, True])

    async def boom(text):
        raise RuntimeError("no device connected")

    io.speak = boom
    asyncio.run(greet_on_arrival(io, sensor, policy, activity))
    assert io.listens == 0  # we never got as far as opening the window


def test_short_absence_is_not_an_arrival():
    io, sensor, policy, activity, _ = greet_setup([True, False, True],
                                                  away_s=60 * 60)
    asyncio.run(greet_on_arrival(io, sensor, policy, activity))
    assert io.spoken == []


def test_activity_window_expires():
    clk = GreetClock()
    act = Activity(window_s=20.0, now=clk)
    assert not act.busy()          # nothing has happened yet
    act.touch()
    assert act.busy()
    clk.advance(21)
    assert not act.busy()


def test_parse_quiet_hours():
    assert parse_quiet_hours("22-8") == (22, 8)
    assert parse_quiet_hours("0-0") == (0, 0)
    with pytest.raises(ValueError):
        parse_quiet_hours("late")
    with pytest.raises(ValueError):
        parse_quiet_hours("22-99")


def test_parse_greetings_defaults_when_unset():
    assert parse_greetings(None) is None
    assert parse_greetings([]) is None


def test_parse_greetings_overrides_only_the_times_you_name():
    table = parse_greetings(["morning:Claws up.", "morning:Morning, Pim."])
    assert table["morning"] == ("Claws up.", "Morning, Pim.")
    # untouched times keep their built-in lines
    assert table["evening"] == DEFAULT_GREETINGS["evening"]


def test_parse_greetings_keeps_colons_in_the_text():
    table = parse_greetings(["evening:Evening: still going?"])
    assert table["evening"] == ("Evening: still going?",)


def test_parse_greetings_rejects_bad_input():
    for bad in (["nonsense"], ["morning:"], ["lunchtime:hello"]):
        with pytest.raises(ValueError):
            parse_greetings(bad)


def test_custom_greetings_reach_the_device():
    io, sensor, policy, activity, _ = greet_setup([True, False, True])
    table = parse_greetings(["morning:Claws up, Pim."])
    asyncio.run(greet_on_arrival(io, sensor, policy, activity, greetings=table))
    assert io.spoken == ["Claws up, Pim."]


def test_script_can_import_the_bridge_package_when_run_directly():
    """Regression: run as a script, Python puts tools/ on sys.path rather than
    the bridge dir, so `from clawlexa_bridge...` inside the presence code blew up
    with ModuleNotFoundError at startup — invisible to these tests, which import
    through pytest's rootdir. Drive the real entry point from an unrelated cwd
    and require the failure to be the *argument* error, not an import one.
    """
    import subprocess
    import pathlib

    script = pathlib.Path(__file__).resolve().parents[1] / "tools" / "voice_agent.py"
    proc = subprocess.run([sys.executable, str(script), "--greeting", "badbucket:x"],
                          capture_output=True, text=True, cwd="/", timeout=60)
    assert "ModuleNotFoundError" not in proc.stderr, proc.stderr[-2000:]
    assert "unknown greeting time" in proc.stderr


def test_failed_greeting_does_not_leave_the_speaking_crab_up():
    """`speaking` is set before the clip plays. If speak/listen then fails, no
    conversation ever opens — and it's the conversation *ending* that returns the
    device to idle. So the failure path has to clear it, or the crab sits there
    looking like it's talking until the next wake word."""
    io, sensor, policy, activity, _ = greet_setup([True, False, True])

    async def boom(text):
        raise RuntimeError("no device connected")

    io.speak = boom
    asyncio.run(greet_on_arrival(io, sensor, policy, activity))
    assert io.states == ["speaking", "idle"]


def test_failed_greeting_survives_a_device_that_cannot_even_be_reset():
    """If the device is entirely gone, the tidy-up set_state fails too — that
    must not escape and kill the watcher."""
    io, sensor, policy, activity, _ = greet_setup([True, False, True])

    async def boom(*a):
        raise RuntimeError("no device connected")

    io.speak = boom
    io.set_state = boom
    asyncio.run(greet_on_arrival(io, sensor, policy, activity))  # must not raise

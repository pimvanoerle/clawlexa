"""Unit tests for the energy-based endpointer — pure logic, synthetic audio."""
import array

from clawlexa_bridge.vad import Endpointer, rms

FRAME_SAMPLES = 320  # 20 ms @ 16 kHz


def voiced(n_frames: int, amp: int = 6000) -> bytes:
    return array.array("h", [amp] * (FRAME_SAMPLES * n_frames)).tobytes()


def silence(n_frames: int) -> bytes:
    return b"\x00\x00" * (FRAME_SAMPLES * n_frames)


def test_rms_silence_vs_tone():
    assert rms(silence(1)) == 0.0
    assert rms(voiced(1, amp=5000)) == 5000.0
    assert rms(b"") == 0.0


def test_silence_only_yields_nothing():
    ep = Endpointer()
    assert ep.feed(silence(50)) == []
    assert ep.flush() is None


def test_speech_then_silence_emits_one_utterance():
    # explicit end_silence so the test is independent of the default hangover
    ep = Endpointer(end_silence_ms=600)  # 30 frames of silence ends an utterance
    out = ep.feed(voiced(10) + silence(30))
    assert len(out) == 1
    # pre-roll(5 captured before start) + 10 voiced - overlap + 30 silence; just
    # assert it's a non-trivial chunk of 16-bit frames.
    assert len(out[0]) % 2 == 0 and len(out[0]) > len(voiced(10))


def test_flush_emits_in_progress_speech():
    ep = Endpointer()
    out = ep.feed(voiced(8))  # no trailing silence yet -> nothing emitted
    assert out == []
    tail = ep.flush()
    assert tail is not None and len(tail) > 0


def test_short_blip_below_start_is_ignored():
    # 3 voiced frames < start_frames(5), then silence -> never starts
    ep = Endpointer()
    assert ep.feed(voiced(3) + silence(30)) == []
    assert ep.flush() is None


def test_two_utterances_separated_by_silence():
    ep = Endpointer(end_silence_ms=600)  # 30 silent frames per gap
    out = ep.feed(voiced(10) + silence(30) + voiced(10) + silence(30))
    assert len(out) == 2


def test_max_utterance_force_emits():
    # continuous speech past max_utterance_ms is cut off rather than buffered forever
    ep = Endpointer(max_utterance_ms=200)  # 10 frames
    out = ep.feed(voiced(40))
    assert len(out) >= 1

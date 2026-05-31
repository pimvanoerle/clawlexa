import os
import wave

from clawlexa_bridge.audio import AudioRecorder


def test_records_a_wav(tmp_path):
    rec = AudioRecorder(outdir=str(tmp_path))
    path = rec.begin(16000, 1, 16)
    pcm = bytes(range(256)) * 4  # 1024 bytes of PCM
    rec.write(pcm)
    rec.write(pcm)
    closed, nbytes = rec.end()

    assert closed == path
    assert nbytes == 2 * len(pcm)
    with wave.open(path, "rb") as w:
        assert w.getframerate() == 16000
        assert w.getnchannels() == 1
        assert w.getsampwidth() == 2
        assert w.readframes(w.getnframes()) == pcm * 2


def test_begin_finalizes_previous_session(tmp_path):
    rec = AudioRecorder(outdir=str(tmp_path))
    p1 = rec.begin(16000, 1, 16)
    rec.write(b"\x00" * 100)
    p2 = rec.begin(16000, 1, 16)  # opening a new one closes the first
    assert p1 != p2
    assert os.path.exists(p1)
    rec.end()


def test_end_when_idle_is_noop():
    rec = AudioRecorder(outdir="/tmp/clawlexa-should-not-be-created-xyz")
    assert rec.end() == (None, 0)
    assert not rec.active()
    assert not os.path.exists("/tmp/clawlexa-should-not-be-created-xyz")

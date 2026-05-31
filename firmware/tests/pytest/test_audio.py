"""On-device check for Phase 1c audio playback bring-up.

Asserts the boot log: the PCM5101A I2S DAC initialized and a test tone played.
Sound itself is a human check; this guards that init/playback didn't error out.
"""


def test_audio_ready_and_plays(dut):
    dut.expect("display: ST77916 ready", timeout=10)
    dut.expect("audio: PCM5101 ready", timeout=5)
    dut.expect("audio: playing", timeout=5)


def test_mic_ready(dut):
    dut.expect("audio: PCM5101 ready", timeout=10)
    dut.expect("mic: ICS-43434 ready", timeout=5)

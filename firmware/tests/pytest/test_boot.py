"""Smoke tests against a real ESP32-S3.

Phase 1a contract: device boots, prints the banner, and emits a heartbeat.
That's the entire surface right now — display/audio bring-up lands in 1b/1c
and gets its own tests.
"""


def test_boot_banner(dut):
    dut.expect("clawlexa booted", timeout=10)
    dut.expect("version : ", timeout=2)
    dut.expect("chip    : ESP32-S3", timeout=2)


def test_heartbeat_emits(dut):
    # Banner happens before heartbeat; consume it so the heartbeat assertion
    # doesn't race against earlier log lines.
    dut.expect("clawlexa booted", timeout=10)
    dut.expect("heartbeat 0", timeout=10)
    dut.expect("heartbeat 1", timeout=10)

"""On-device checks for Phase 1b display + touch bring-up.

We assert the observable boot log — the panel and touch controllers reporting
ready. Pixels can't be checked over serial, so the visible "hello" stays a
human check; these tests guard that init didn't regress or error out.
"""


def test_display_ready(dut):
    dut.expect("clawlexa booted", timeout=10)
    # Panel-variant detection logs which of the two ST77916 boards this is.
    dut.expect("panel variant:", timeout=10)
    dut.expect("display: ST77916 ready", timeout=10)


def test_touch_ready(dut):
    dut.expect("display: ST77916 ready", timeout=10)
    dut.expect("touch: CST816 ready", timeout=5)

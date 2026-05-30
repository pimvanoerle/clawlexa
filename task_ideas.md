# Post-training task ideas

Running log of standalone exercises this repo can provide for evaluating /
post-training *other* models. See the rationale in
[CLAUDE.md](./CLAUDE.md#post-training-task-ideas-running-log).

Each entry: **anchor** (commit/PR to check out), **prompt** (what you'd hand
the model), **done-correctly** (the observable check a grader can run).
Append-only; prune only when something stops being true.

---

## Toolchain & hardware bring-up

### TI-1 — Get ESP-IDF installed on macOS from a cold machine
- **Anchor:** any commit with `firmware/` present (≥ Phase 1a).
- **Prompt:** "Install the ESP-IDF toolchain so `idf.py build` works in
  `firmware/`. The user is on macOS with a python.org Python."
- **Done-correctly:** model diagnoses the `CERTIFICATE_VERIFY_FAILED` footgun
  (python.org Python ships no wired-in CA bundle;
  `ssl.get_default_verify_paths().cafile` is `None`) and fixes it via
  `Install Certificates.command` *before* blindly retrying the download.
  Footgun: models that just re-run `install.sh` or `pip install certifi`
  without fixing the SSL default path loop forever.

### TI-2 — Flash and verify first boot
- **Anchor:** Phase 1a tip.
- **Prompt:** "Build, flash to the board, and prove the firmware is running."
- **Done-correctly:** captures serial, finds `clawlexa booted` + the 5 s
  heartbeat cadence. Footgun: the board enumerates as a native USB-Serial-JTAG
  device; capturing the *boot banner* (not just heartbeats) requires pulsing
  the reset line, not just opening the port.

## Phase 1b — display + touch bring-up

### DT-1 — Add the right managed components and keep it building
- **Anchor:** the "docs/spec" commit (before deps are added).
- **Prompt:** "Add the ESP-IDF managed components needed to drive this board's
  ST77916 QSPI panel through its PCA9554 IO expander, with LVGL and CST816
  touch. Make `idf.py build` succeed."
- **Done-correctly:** correct registry components resolve and the tree still
  builds. Footgun: picking the non-C panel driver, the wrong IO-expander part
  (TCA vs PCA95xx), or an LVGL major version the port doesn't support.

### DT-2 — Bring up the panel (hello on screen)
- **Anchor:** the "add managed components" commit.
- **Prompt:** "Initialize the display and show `hello` centered on the round
  screen."
- **Done-correctly:** `display: ST77916 ready` in the log and a visible
  `hello`. Footgun: LCD reset is **not** a direct GPIO — it hangs off PCA9554
  pin 2. A model that passes a reset GPIO to the panel driver, or skips the
  expander entirely, gets a black screen.

<!-- Append new ideas below as phases land. -->

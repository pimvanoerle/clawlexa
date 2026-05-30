# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

clawlexa is a hardware peripheral that gives Claude-powered agents voice,
hearing, and a touch screen. The device is a Waveshare ESP32-S3-Touch-LCD-1.85C.
A host-side bridge exposes an **MCP server** so any compatible agent can plug
in. The first consumer is iPinch (a homebrew Claude Code wrapper that runs on
a laptop and uses Slack as its chat surface), but the design must stay
agent-agnostic.

**Always read [SPEC.md](./SPEC.md) before making design choices.** It is the
source of truth for what's decided vs. still open, and lists every pending
decision in §14. Update SPEC.md whenever a decision gets made or reversed.

## Architecture in one line

`ESP32 ──WiFi──> host bridge ──MCP──> agent`. The device handles the wake word
locally so it isn't streaming audio constantly; everything heavier (STT, TTS,
agent loop) happens on the host.

## Working norms for this repo

- **Spec-first.** When the user asks for a new capability, first check whether
  SPEC.md captures the intent. If not, propose a spec edit before writing
  code. If the change resolves an item in §14, mark it Decided in the same PR.
- **Don't lock in the MVP path prematurely.** iPinch is the first consumer but
  not the only one — keep the MCP surface free of iPinch-specific assumptions.
- **Firmware and bridge are independent code trees.** Changes that span both
  should be obvious in the diff (e.g. the wire protocol changing) and called
  out in the commit message.
- **Don't fabricate file paths, build commands, or test commands** in this
  file until they actually work. The `firmware/` tree exists as of Phase 1a;
  the `bridge/` tree does not yet — don't document it until it lands.

## Post-training task ideas (running log)

This repo doubles as a source of post-training exercises for *other* models.
Bring-up work on this board has a lot of cross-model footguns (the C-vs-non-C
pinout, the QSPI panel, the IO-expander-gated reset, ESP-IDF version drift),
which makes "check out commit `<sha>` and get the Waveshare board up" a strong,
verifiable eval.

So, while implementing phases:

- **Split work into self-contained commits that each build and pass tests on
  their own.** A reviewer model should be able to `git checkout <sha>`, run the
  documented build/test commands, and land in a known-good state. Call out in
  the commit message what state that commit leaves the tree in.
- **Prefer commit boundaries that make good standalone tasks** — "add these
  managed components and make it build", "bring up the panel", "make the host
  tests green", "refactor IO out of the testable core". These are exactly the
  shapes that exercise a model well.
- **Log candidate exercises in [task_ideas.md](./task_ideas.md)** as you go:
  the anchor commit (or "after this PR"), the prompt you'd give the model, and
  what "done correctly" looks like (the observable check). Keep it append-only;
  prune only things that stopped being true.

## Commands

**Firmware build & flash** (ESP-IDF v5.4+, must be sourced first):

```bash
cd firmware
idf.py set-target esp32s3      # one-time per checkout
idf.py build
idf.py -p <PORT> flash monitor # <PORT> is the USB serial device
```

**Host unit tests** (no board, no ESP-IDF needed — these run in CI):

```bash
cmake -B firmware/tests/host/build -S firmware/tests/host
cmake --build firmware/tests/host/build
ctest --test-dir firmware/tests/host/build --output-on-failure
```

**On-device integration tests** (board plugged in, firmware built):

```bash
cd firmware
idf.py build
pytest tests/pytest
```

The `tests/pytest/` suite uses `pytest-embedded` — install its deps into a
venv first per `firmware/tests/README.md`.

## Testing rules (Phase 1+)

- Any logic that *can* be tested host-side (no IDF headers, no FreeRTOS, no
  IO) *must* be — extract it from IO modules until it can.
- Add or update at least one test per PR that isn't documentation-only.
- On-device tests assert observable behavior (log lines, serial responses),
  never internals.
- Several boot-log literals are part of the on-device test contract — don't
  rename one without updating the test that asserts it:
  - `"clawlexa booted"` (`main/main.c`) → `tests/pytest/test_boot.py`
  - `"display: ST77916 ready"`, `"panel variant:"` (`main/display.c`) →
    `tests/pytest/test_display.py`
  - `"touch: CST816 ready"` (`main/touch.c`) → `tests/pytest/test_display.py`

## Hardware notes

The board is the **"C" variant** of the Waveshare ESP32-S3-Touch-LCD-1.85
family — it includes the onboard mic and speaker. The plain (non-C) variant
does **not** have audio. If a doc, library, or pinout you find references the
non-C variant, double-check before trusting pin assignments.

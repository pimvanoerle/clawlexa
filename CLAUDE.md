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
- **No code exists yet.** This is Phase 0. Don't fabricate file paths, build
  commands, or test commands in this file until they actually work. Update
  this section the moment the first `firmware/` or `bridge/` project lands.

## Commands

_None yet — repo is at Phase 0 (scaffolding). Populate this section once the
firmware project and bridge are bootstrapped._

## Hardware notes

The board is the **"C" variant** of the Waveshare ESP32-S3-Touch-LCD-1.85
family — it includes the onboard mic and speaker. The plain (non-C) variant
does **not** have audio. If a doc, library, or pinout you find references the
non-C variant, double-check before trusting pin assignments.

# clawlexa

<p align="center">
  <img src="docs/clawlexa-crab.svg" alt="clawlexa mascot — a happy cartoon crab waving" width="240">
</p>

A voice, hearing, and touch interface for Claude-powered agents — built on the
Waveshare ESP32-S3-Touch-LCD-1.85C.

clawlexa runs as an MCP server on the same host as your agent, so anything that
speaks MCP (iPinch, ourclaw, spark, Claude Desktop, …) can give itself a face
and a voice by connecting to it.

Status: **Phase 1a** — firmware scaffolded, boots to UART banner, test
framework live; display + audio next. See [SPEC.md](./SPEC.md) for the design,
[firmware/README.md](./firmware/README.md) for build/flash, and
[CLAUDE.md](./CLAUDE.md) for working-with-Claude-Code notes.

# Post-training task ideas

Running log of standalone exercises this repo can provide for evaluating /
post-training *other* models. See the rationale in
[CLAUDE.md](./CLAUDE.md#post-training-task-ideas-running-log).

Each entry: **anchor** (commit/PR to check out), **prompt** (what you'd hand
the model), **done-correctly** (the observable check a grader can run), and a
**tier** marking how it can be graded. Append-only; prune only when something
stops being true.

## Verification tiers

Most footguns split into a *decision/logic core* (gradeable with no board) and a
*physical effect* (needs the board). Tag each task with the tier(s) it can be
graded at, so a grader runs the slice that fits the environment. Tiers 1–4 run
headless in Docker (`espressif/idf` image); only tier 5 needs hardware.

- **T1 Build** — `idf.py build` for esp32s3 succeeds. Catches wrong/missing
  components, bad APIs, CMake/requires errors.
- **T2 Host-logic** — pure C unit tests (Unity, `tests/host/`). Catches decision
  logic: variant selection, geometry, parsers, state machines.
- **T3 Host-mock** — link firmware logic against fake IDF driver headers, assert
  call *sequence/arguments*. Catches ordering (reset-before-init,
  `reset_gpio_num=-1`, right init array, clear-before-LVGL). *(harness TBD)*
- **T4 QEMU** — `pytest-embedded-qemu` boot smoke. Catches panics / early-log
  regressions for logic-only firmware. Note: QEMU has no ST77916/CST816 model,
  so the display/touch path only runs here if guarded behind a fake board impl.
- **T5 Device** — real board. The only tier that confirms actual pixels/taps.

When a task is T5-only at face value, decompose it: list the device-free slices
(T1–T4) separately so CI grades the parts a model most often gets wrong.

---

## Toolchain & hardware bring-up

### TI-1 — Get ESP-IDF installed on macOS from a cold machine
- **Tier:** T5 (host machine state; not Dockerable — it's about the dev's mac).
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
- **Tier:** T5 device (real boot) — but T4 QEMU can grade the `clawlexa booted`
  banner headless, since Phase 1a firmware is logic-only (no peripheral init).
- **Anchor:** Phase 1a tip.
- **Prompt:** "Build, flash to the board, and prove the firmware is running."
- **Done-correctly:** captures serial, finds `clawlexa booted` + the 5 s
  heartbeat cadence. Footgun: the board enumerates as a native USB-Serial-JTAG
  device; capturing the *boot banner* (not just heartbeats) requires pulsing
  the reset line, not just opening the port.

## Phase 1b — display + touch bring-up

### DT-1 — Add the right managed components and keep it building
- **Tier:** T1 (fully device-free — the whole task is "make it build").
- **Anchor:** the "docs/spec" commit (before deps are added).
- **Prompt:** "Add the ESP-IDF managed components needed to drive this board's
  ST77916 QSPI panel through its PCA9554 IO expander, with LVGL and CST816
  touch. Make `idf.py build` succeed."
- **Done-correctly:** correct registry components resolve and the tree still
  builds. Footgun: picking the non-C panel driver, the wrong IO-expander part
  (TCA vs PCA95xx), or an LVGL major version the port doesn't support.

### DT-2 — Bring up the panel (hello on screen)
- **Tier:** T5 for the visible `hello`; T1 (builds) + T3 (reset sequenced via
  expander before panel init, `reset_gpio_num=-1`) device-free.
- **Anchor:** the "add managed components" commit.
- **Prompt:** "Initialize the display and show `hello` centered on the round
  screen."
- **Done-correctly:** `display: ST77916 ready` in the log and a visible
  `hello`. Footgun: LCD reset is **not** a direct GPIO — it hangs off PCA9554
  pin 2. A model that passes a reset GPIO to the panel driver, or skips the
  expander entirely, gets a black screen.

### DT-3 — The two-panel-variant init footgun (the hard one)
- **Anchor:** "add managed components" commit (panel not yet working).
- **Prompt:** "Get a clean image on the screen — the panel currently shows
  scrambled stripes."
- **Done-correctly:** crisp, stable content with a clean background. This is a
  layered trap that mirrors a real bring-up session:
  1. The generic `esp_lcd_st77916` default init does NOT drive this panel —
     output is scrambled. You must supply Waveshare's vendor `init_cmds`.
  2. **There are TWO panel variants on this board**, needing *different* init
     arrays. Waveshare's demo picks between them by reading panel register
     `0x04` over a slow (3 MHz) IO before re-opening at full clock. A model that
     hardcodes one array has a ~50% chance of a still-scrambled panel. Correct
     answer replicates the register probe (our board reports `00 02 7f 7f` =
     "new" variant, which also needs INVON `0x21`).
  3. LVGL's partial draw buffer only paints dirty regions, so power-on GRAM
     garbage shows through the undrawn background until you explicitly clear the
     panel to black once.
- **Grader signal:** the boot log prints `panel reg 0x04 = ...` and
  `panel variant: <new|default>`; correct firmware selects to match the board.
- **Source of truth:** waveshareteam/ESP32-S3-Touch-LCD-1.85C, `main/LCD_Driver/`.
- **Note:** a residual very-faint mura band on pure black at screen center is a
  physical panel trait (a solid color fill is perfectly clean) — NOT a bug to
  chase. Models that rabbit-hole on it are over-fitting.
- **Device-free decomposition** (grade the parts models get wrong, no board):

  | Slice | Tier | Grader check |
  |-------|------|--------------|
  | Components added, builds for esp32s3 | T1 | `idf.py build` exits 0 |
  | Variant decision: `st77916_variant_from_id()` maps `{00,02,7f,7f}`→NEW, others→DEFAULT | T2 | `ctest` → `test_st77916_variant` green |
  | Off-glass touches dropped: `touch_in_circle()` | T2 | `ctest` → `test_touch_geom` green |
  | Call order: expander reset → `reset_gpio_num=-1` → selected init array → GRAM clear → LVGL | T3 | mock records expected call sequence *(harness TBD)* |
  | Crisp `hello`, no stripes | T5 | human / device |

  As of the "extract variant decision" commit, slices 1–2 are live: the variant
  core is a pure function (`main/st77916_variant.c`) with a host test, so the
  central DT-3 footgun is gradeable with zero hardware.

<!-- Append new ideas below as phases land. -->

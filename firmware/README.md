# clawlexa firmware

ESP-IDF project targeting the Waveshare ESP32-S3-Touch-LCD-1.85**C** (audio
variant — the plain non-C board has no mic/speaker).

## Phase status

We're at **Phase 1a**: project scaffolded, boots to a UART banner + heartbeat,
test framework live. **No display or audio drivers yet** — that's 1b and 1c.

## One-time setup

### 1. Install ESP-IDF v5.4+

If you don't have it:

```bash
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 -b v5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Source the environment in every shell you build from:

```bash
. ~/esp/esp-idf/export.sh
```

> Tip: add `alias get_idf='. ~/esp/esp-idf/export.sh'` to your shell rc.

### 2. Editor sanity (clangd / VS Code C++)

LSP errors about missing `freertos/FreeRTOS.h` etc. mean your editor doesn't
have a compile database. Build once after sourcing IDF:

```bash
cd firmware
idf.py reconfigure
```

This generates `build/compile_commands.json`. Most editors (VS Code with
clangd extension, Neovim with clangd LSP, Cursor) pick it up automatically
if you symlink it to repo root, or you can point clangd at it directly.

## Build & flash

```bash
cd firmware
idf.py set-target esp32s3   # one-time per checkout
idf.py build
idf.py -p <PORT> flash monitor
```

`<PORT>` is your USB serial device:
- macOS: `/dev/cu.usbmodem*` or `/dev/cu.usbserial-*`
- Linux: `/dev/ttyACM0` or `/dev/ttyUSB0`

If the board doesn't enter download mode automatically, hold the **BOOT**
button while pressing **RESET**, then release BOOT.

You should see something like:

```
I (314) clawlexa: clawlexa booted
I (314) clawlexa:   version : 0.1.0
I (314) clawlexa:   chip    : ESP32-S3 rev 0, 2 cores
I (324) clawlexa:   psram   : 8388608 bytes
I (5324) clawlexa: heartbeat 0
I (10324) clawlexa: heartbeat 1
```

Exit `monitor` with **Ctrl-]**.

## Tests

See [tests/README.md](tests/README.md) for the full breakdown.

Short version:

```bash
# Host unit tests (no board needed)
cmake -B firmware/tests/host/build -S firmware/tests/host
cmake --build firmware/tests/host/build
ctest --test-dir firmware/tests/host/build --output-on-failure

# On-device integration tests (board plugged in)
cd firmware && idf.py build
pytest tests/pytest
```

## Layout

```
firmware/
├── CMakeLists.txt              # project root
├── sdkconfig.defaults          # board-independent defaults
├── sdkconfig.defaults.esp32s3  # ESP32-S3 + Waveshare board defaults
├── partitions.csv              # 16 MB layout with OTA + storage
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml       # managed component deps (grows per phase)
│   ├── main.c                  # app_main, boot banner, heartbeat
│   ├── app_version.{c,h}       # compile-time version (single source of truth)
│   └── include/
└── tests/
    ├── host/                   # Unity host tests, FetchContent for Unity
    └── pytest/                 # pytest-embedded device tests
```

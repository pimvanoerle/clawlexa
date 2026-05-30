# clawlexa firmware tests

Two layers (see SPEC.md §10a):

| Layer | Where | Runs on | When |
|-------|-------|---------|------|
| Host unit tests (Unity) | `host/` | Any Linux/macOS box, no board | Every push, CI |
| On-device integration tests (pytest-embedded) | `pytest/` | Real ESP32-S3 over USB-serial | Pre-merge, by humans |

## Layer 1 — host unit tests

Pure C modules with no ESP-IDF dependencies. The host CMake project pulls
Unity via FetchContent and compiles the modules under test with the system
compiler.

```bash
cd firmware/tests/host
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Add new tests by:
1. Dropping `test_<thing>.c` into `host/` with one or more `void test_<name>(void)` functions.
2. Adding the test file and any source files under test to `host/CMakeLists.txt`.
3. Adding `RUN_TEST(test_<name>);` calls to `host/test_runner.c`.

**Rule:** if a piece of logic *can* be exercised here (no IO, no FreeRTOS,
no IDF headers), it *must* be.

## Layer 2 — on-device integration tests

`pytest-embedded` flashes a fresh build to a connected board and asserts on
serial output. Run after physical changes, before merging anything that
touches firmware boot or peripheral init.

```bash
# one-time setup
python -m venv firmware/tests/pytest/.venv
source firmware/tests/pytest/.venv/bin/activate
pip install -r firmware/tests/pytest/requirements.txt

# build firmware once, then run tests
cd firmware
idf.py build
pytest tests/pytest
```

By default pytest-embedded auto-detects the connected ESP32. If you have
multiple boards plugged in:

```bash
pytest tests/pytest --port /dev/cu.usbmodem1101
```

Tests assert *observable behavior* (log lines, serial responses), never
internals. Don't grep for variable names or memory addresses — those are
not contracts.

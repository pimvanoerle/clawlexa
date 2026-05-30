# ESP32-S3-Touch-LCD-1.85**C** — pinout

Confirmed pin assignments for the **C variant** (onboard mic + speaker). The
plain (non-C) board differs — do **not** trust a non-C pinout here.

`firmware/main/board.h` is the single source of truth in code; this file is the
human-readable reference. Keep them in sync.

## Display — ST77916, 360×360 round IPS, QSPI

| Signal | ESP32-S3 |
|--------|----------|
| CS     | GPIO21   |
| CLK    | GPIO40   |
| D0     | GPIO46   |
| D1     | GPIO45   |
| D2     | GPIO42   |
| D3     | GPIO41   |
| RESET  | **PCA9554 expander pin 2** (not a direct GPIO) |
| Backlight | GPIO5 (PWM) |

> **Footgun:** LCD reset is driven through the PCA9554 IO expander, not an MCU
> GPIO. The init sequence must bring the expander up over I²C, pulse the reset
> line, *then* init the ST77916. Pass `reset_gpio_num = -1` to the panel driver.

## Touch — CST816, capacitive, I²C

| Signal | Value |
|--------|-------|
| I²C address | 0x15 |
| INT    | GPIO4 |
| RESET  | via PCA9554 (TBD which pin — verify against schematic) |

## I²C bus (shared)

| Signal | ESP32-S3 |
|--------|----------|
| SDA    | GPIO11   |
| SCL    | GPIO10   |

Devices on the bus: CST816 touch (0x15), PCA9554 IO expander (0x20). The
QMI8658 IMU, RTC, and audio codec also sit on this board's I²C — addresses TBD,
documented when those phases land.

## Audio (Phase 1c — not yet wired)

| Signal | ESP32-S3 |
|--------|----------|
| Speaker I²S DOUT | GPIO47 |
| (mic + codec pins) | TBD |

## Sources & caveats

These pins are cross-checked against community configs for the **1.85C**
specifically, not the variant-mixed forum posts for sibling boards:

- Waveshare wiki: <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C>
- ESPHome config for the C variant:
  <https://github.com/ulsmith/home-assistant-esphome-esp32-s3-touch-lcd-185c>

**Before trusting any pin for new IO, confirm against the official Waveshare
schematic/demo.** Several values above (touch reset pin, audio codec pins) are
still marked TBD precisely because they haven't been verified on hardware yet.
Update this table as each is confirmed by a working flash.

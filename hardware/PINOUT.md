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

## Audio — ES8311 (play) + ES7210 (capture), I²S + shared I²C

Both codecs sit on the shared I²C bus (SCL 10 / SDA 11) and share one I²S bus.

| Signal | ESP32-S3 |
|--------|----------|
| I²S MCLK | GPIO2 |
| I²S BCLK (SCLK) | GPIO48 |
| I²S WS (LRCK) | GPIO38 |
| I²S DOUT (→ ES8311 → speaker) | GPIO47 |
| I²S DIN (← ES7210 mic ADC) | GPIO39 |
| Speaker amp (PA) enable | GPIO15 |

| I²C device | Addr |
|------------|------|
| ES8311 playback codec | 0x18 |
| ES7210 capture ADC | 0x40 |

> **Footgun (PA):** the speaker amplifier is gated by GPIO15 — the demo drives it
> high around playback. No PA enable ⇒ silent speaker even with a working codec.
>
> **Caveat (verify on hardware):** these come from Waveshare's official 1.85C
> demo, whose audio-board config is **KORVO-2-derived** (a 4-mic ES7210 array).
> The pins look right, but confirm the codecs by I²C-scanning for 0x18/0x40 at
> bring-up before trusting them; a one-mic round board may differ.

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

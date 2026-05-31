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

## Audio — PCM5101A DAC (play) + ICS-43434 mic (capture), pure I²S

Verified against the official board **schematic**. There is **NO I²C audio
codec** on this board — audio is two independent I²S devices, no control bus.

**Speaker** — PCM5101A I²S DAC → NS8002 amplifier (DAC runs MCLK-less via its
internal PLL off BCLK):

| Signal | ESP32-S3 |
|--------|----------|
| I²S BCLK | GPIO48 |
| I²S WS (LRCK) | GPIO38 |
| I²S DOUT (→ PCM5101A DIN) | GPIO47 |

**Microphone** — ICS-43434 I²S MEMS mic, on its **own** I²S bus:

| Signal | ESP32-S3 |
|--------|----------|
| I²S SCK | GPIO15 |
| I²S WS | GPIO2 |
| I²S SD (← mic data) | GPIO39 |

> **Footgun (the big one):** Waveshare's ESP-IDF *demo* configures **ES8311 +
> ES7210 over I²C** (its audio-board component is KORVO-2 copy-paste). Those
> chips **are not on this board** — an I²C scan finds nothing at 0x18/0x40, and
> chasing the demo wastes hours looking for a codec that doesn't exist. Trust the
> schematic: PCM5101A (no I²C) for playback, ICS-43434 (no I²C) for capture.

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

#pragma once

/* Pin map for the Waveshare ESP32-S3-Touch-LCD-1.85C (the "C" variant, with
 * onboard audio). Mirror of hardware/PINOUT.md — keep the two in sync. Do not
 * trust non-C pinouts here; several signals differ across the family.
 *
 * Footgun worth repeating: the LCD reset line is NOT an MCU GPIO. It hangs off
 * the PCA9554 IO expander (pin 2). Bring the expander up over I2C and pulse
 * reset there before touching the ST77916; pass reset_gpio_num = -1 to the
 * panel driver. */

/* Shared I2C bus: touch + IO expander (+ IMU/RTC/codec, later phases). */
#define BOARD_I2C_SDA_GPIO        11
#define BOARD_I2C_SCL_GPIO        10
#define BOARD_I2C_FREQ_HZ         400000

/* PCA9554 IO expander (TCA9554-register-compatible). */
#define BOARD_IO_EXPANDER_ADDR    0x20
#define BOARD_EXIO_LCD_RESET      2     /* expander pin driving LCD reset */

/* ST77916 round 360x360 panel over QSPI. */
#define BOARD_LCD_H_RES           360
#define BOARD_LCD_V_RES           360
#define BOARD_LCD_QSPI_CS_GPIO    21
#define BOARD_LCD_QSPI_CLK_GPIO   40
#define BOARD_LCD_QSPI_D0_GPIO    46
#define BOARD_LCD_QSPI_D1_GPIO    45
#define BOARD_LCD_QSPI_D2_GPIO    42
#define BOARD_LCD_QSPI_D3_GPIO    41
#define BOARD_LCD_BL_GPIO         5     /* backlight, PWM-capable */

/* CST816 capacitive touch on the shared I2C bus. */
#define BOARD_TOUCH_ADDR          0x15
#define BOARD_TOUCH_INT_GPIO      4

/* Audio is pure I2S — NO I2C codec on this board (the demo's ES8311/ES7210
 * config is KORVO-derived and does not apply). Speaker: PCM5101A I2S DAC into
 * an NS8002 amp. Mic: ICS-43434 I2S MEMS mic on its own I2S bus. */
#define BOARD_SPK_I2S_BCLK_GPIO   48
#define BOARD_SPK_I2S_WS_GPIO     38
#define BOARD_SPK_I2S_DOUT_GPIO   47    /* -> PCM5101A DIN -> speaker */
#define BOARD_MIC_I2S_SCK_GPIO    15
#define BOARD_MIC_I2S_WS_GPIO     2
#define BOARD_MIC_I2S_SD_GPIO     39    /* <- ICS-43434 data out */

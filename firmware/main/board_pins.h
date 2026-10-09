/* board_pins.h — the boards one image runs on, told apart at boot over I2C
 * (display_port_init; SDA / SCL, the QSPI data lines and LCD_CS are shared):
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1: SH8601 + FT3168; V2: CO5300 +
 * CST816). Sources: Waveshare esp-idf examples and the official Arduino
 * variant. VERIFY I2C SDA/SCL on the bench: Waveshare's own code says
 * SDA=15/SCL=14, the Arduino variant says the reverse.
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C (2026-10-01; ROUND 466x466 CO5300 +
 * CST9217). Sources: resources/ESP32-S3-Touch-AMOLED-1.75C/ (the schematic's
 * GPIO table) and Waveshare's BSP (waveshare/esp32_s3_touch_amoled_1_75c).
 * No IO expander - the resets are GPIOs - no RTC chip, no SD card; an ES7210
 * microphone ADC the 1.8 does not have (its I2C address is how the board is
 * recognized). PMIC, IMU, codec, amp and every audio pin: as on the 1.8.
 *
 * Waveshare ESP32-S3-Touch-AMOLED-2.06, the WATCH (2026-10-02; 410x502
 * CO5300 + FT3168, the 1.8's panel family a size up; worn, so its own build
 * is a PORTRAIT 410x502 tank, the panel unturned). Sources: resources/ESP32-S3-Touch-AMOLED-2.06-Watch/ (schematic,
 * wiki page) and Waveshare's BSP (waveshare/esp32_s3_touch_amoled_2_06).
 * No IO expander: the resets are GPIOs 8 and 9 - the pins the 1.8 plays its
 * I2S bit clock and data on, so the AUDIO PINS differ here (audio_port) and
 * the 1.8's would hold the panel in reset. An ES7210 like the 1.75C's, and an
 * RTC chip unlike it: no expander + ES7210 + RTC = the watch. DSI_PWR_EN is
 * not a GPIO: it is pulled up to ALDO2, so that rail IS the panel's power
 * switch. PWR key sense on GPIO 10 (SYS_OUT), an SD slot (1/2/3/17) and a
 * vibration motor (GPIO 18, fed from ALDO3) the tank does not use. */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* ---- the ESP32-S3-Touch-LCD-2.8-V2 (Waveshare; ST7789T + CST328) ----
 * A different board family from the AMOLED ones above: an SPI ST7789T panel,
 * a CST328 touch controller on its own I2C bus, a PCM5101 I2S DAC (no codec),
 * a plain ADC battery meter and a GPIO power-hold latch. The pins below are
 * the ones POCKET_TANK_PORT_PLAN.md section 3 lists; the display/touch/audio
 * ports use them from M1 on. Selected by CONFIG_POCKET_TANK_LCD28. */
#if defined(CONFIG_POCKET_TANK_LCD28)
#define L28_PIN_LCD_SCLK  40
#define L28_PIN_LCD_MOSI  45
#define L28_PIN_LCD_DC    41
#define L28_PIN_LCD_CS    42
#define L28_PIN_LCD_RST   39
#define L28_PIN_LCD_BL    5
#define L28_PIN_I2C_SCL   10        /* main I2C bus (port 0): RTC + IMU */
#define L28_PIN_I2C_SDA   11
#define L28_PIN_TP_SCL    3         /* touch I2C bus (port 1, owned by the touch port) */
#define L28_PIN_TP_SDA    1
#define L28_PIN_TP_INT    4         /* unused: the touch port polls */
#define L28_PIN_TP_RST    2
#define L28_PIN_I2S_BCLK  48
#define L28_PIN_I2S_WS    38
#define L28_PIN_I2S_DOUT  47        /* no MCLK on this board */
#define L28_PIN_BAT_ADC   8         /* ADC1_CH7 */
#define L28_PIN_KEY       6         /* PWR key, active low */
#define L28_PIN_HOLD      7         /* power-hold latch: high = battery connected */
#define L28_PANEL_W       240       /* native portrait */
#define L28_PANEL_H       320
#define L28_I2C_ADDR_CST328  0x1A
#define L28_I2C_ADDR_CST3530 0x58   /* fallback touch controller */

/* the power-hold latch, driven as the very first line of app_main
 * (battery_port_adc.c) */
void board_early_init(void);
#endif

#define PIN_LCD_CS        12
#define PIN_LCD_PCLK      11
#define PIN_LCD_DATA0     4
#define PIN_LCD_DATA1     5
#define PIN_LCD_DATA2     6
#define PIN_LCD_DATA3     7
#define PIN_I2C_SDA       15
#define PIN_I2C_SCL       14
#define PIN_TP_INT        21
#define I2C_ADDR_EXPANDER 0x20     /* TCA9554: bit0 LCD_RST, bit1 DSI_PWR_EN, bit2 TOUCH_RST, bit7 SD_CS */
#define I2C_ADDR_FT3168   0x38     /* V1 touch */
#define I2C_ADDR_CST816   0x15     /* V2 touch (probe => V2 board) */
#define PANEL_W           368      /* native portrait */
#define PANEL_H           448
#define V2_PANEL_X_GAP    0x10

/* ---- the 1.75C ---- */
#define R_PIN_LCD_PCLK    38
#define R_PIN_LCD_RST     1
#define R_PIN_LCD_TE      13       /* unused */
#define R_PIN_TP_RST      2
#define R_PIN_TP_INT      11
#define R_PIN_IMU_INT1    21       /* unused */
#define R_PIN_PWR_SENSE   3        /* SYS_OUT: high while the PWR key is down (unused: the PMIC is asked, as on the 1.8) */
#define I2C_ADDR_ES7210   0x40     /* the microphone ADC (probe, with no expander => the 1.75C) */
#define I2C_ADDR_CST9217  0x5A
#define R_PANEL           466      /* round: 466 across, the corners of the square are not there */
#define R_PANEL_X_GAP     6

/* ---- the 2.06 watch ---- */
#define W_PIN_LCD_RST     8
#define W_PIN_TP_RST      9
#define W_PIN_TP_INT      38       /* unused: polled, like the others */
#define W_PIN_LCD_TE      13       /* unused */
#define W_PIN_PWR_SENSE   10       /* SYS_OUT: high while the PWR key is down */
#define W_PIN_I2S_BCLK    41
#define W_PIN_I2S_DOUT    40       /* ESP -> codec DSDIN */
#define W_PIN_MOTOR       18       /* unused (pulled off on the board) */
#define I2C_ADDR_RTC      0x51     /* PCF85063: the 1.8 and the watch have one, the 1.75C does not */
#define W_PANEL_W         410      /* native portrait */
#define W_PANEL_H         502
#define W_PANEL_X_GAP     0x16
#endif

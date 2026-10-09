# Porting `pocket-tank-main` to the ESP32-S3-Touch-LCD-2.8-V2

> Companion to `README.md` (Part D, the generic porting guide). This is the concrete
> plan for the one project you uploaded: **pocket-tank** (the "pocket fish tank"),
> currently targeting Waveshare AMOLED boards, to be run on the
> **ESP32-S3-Touch-LCD-2.8-V2** (ST7789T 240x320 + CST328 + QMI8658 + PCF85063 +
> PCM5101 + ADC battery).
>
> Status: **M1-M6 delivered on hardware (2026-10-09): the whole port is working —
> colours, touch, battery/PWR key, audio, IMU/RTC axes and deep-sleep wake.** M0's stub
> display is replaced by the real ST7789T panel (stretch: full 320x240, ~25 fps, panel up at
> boot) — the tank is on the 2.8" glass — and the display port's main I2C bus brought
> the PCF85063 RTC and the QMI8658 IMU online with it (both answered on the bench).
> M2 touch port builds, runs and is calibrated: **CST3530 @0x58** (no CST328 @0x1A),
> every touch feature verified on the glass (fish cards, double-taps, feed, wipe
> algae) after the END-ack byte-order fix (§11.2 bench note). M3: the ADC battery
> (~4110 mV / ~92%) plus the PWR key (GPIO6) verified — short press sleeps, a press
> wakes; hold latch GPIO7 in `board_early_init`. M4: **the PCM5101 I2S DAC plays the
> full sound bank** (user-confirmed audible), cold-cycling in 19 ms with the idle
> shutdown. M5: **the IMU's upright axis measured −Y on the bench (matches the port
> default — no calibration change) and the user confirmed the screen rotates 180°
> when the board is turned over with touch staying accurate; the PCF85063 RTC seeds
> the wall clock.** M6: **the PWR key is the deep-sleep ext1 wake — bench-proven
> (`wake from deep sleep (the PWR key)`), with BOOT kept as a backup.** Colour night resolved
> on the bench: this glass's inversion polarity is REVERSED (0x20 inverts, 0x21 is
> normal) and the wire is native RGB565 (no byte swap) — see §11.2 bench note.
> Every source edit so far is inside `pocket-tank-main`;
> nothing under the vendor `ESP-IDF` demo (this repo's `main/`) was modified — the
> ST7789T panel driver was copied into the port tree, unmodified. OTA stays unsigned
> (the signing key is not on this machine); performance is 25.5 fps, above the floor.

---

## 0. Verdict up front

**Feasible, and much easier than a normal port — the project was built to be re-targeted.**

`pocket-tank` is not a monolithic app; its hardware access is behind a small set of
**port seams** (`display_port`, `touch_port`, `audio_port`, `battery_port`, `imu_port`,
`rtc_port`, `codec_port`). Each supported board is one implementation of each seam.
Porting to this board means **writing four new seam implementations and a board entry**,
not rewriting the game.

The two things that are genuinely new work:

1. **Display geometry** — the tank renders a **448x368 landscape** RGB565 frame; our
   panel is **240x320 portrait** (320x240 landscape). The frame must be `FIT`-scaled
   down, in the display port, every frame.
2. **Touch** — our board's CST328 driver (and its odd portrait/landscape axis config)
   must be adapted into the touch seam, then calibrated on the bench.

Two big lucky breaks:

- The **QMI8658 IMU and PCF85063 RTC are the exact same chips at the exact same I2C
  addresses** as the project already supports — those two ports are reusable almost
  verbatim.
- The **SoC is identical** to the project's target: ESP32-S3R8, 8 MB octal PSRAM,
  16 MB flash, 240 MHz. The memory/PSRAM/partition plan carries over unchanged.

The vendor `main/` BSP is **not** used as a base (pocket-tank talks to `esp_lcd` and the
new `i2c_master` driver directly). We only **reuse two things from it**: the ST7789T
panel driver (`Vernon_ST7789T.c/.h`) and the CST328 register protocol.

---

## 1. What pocket-tank is (the 60-second architecture)

```
pocket-tank-main/
  common/                     platform-free: game logic + SOFTWARE renderer
    tank.c  render.c  audio.c  progression.c  setup.c  notice.c  battery.c  update.c
    render.h  tank.h  audio.h ...      -> render.c draws a raw RGB565 framebuffer
  firmware/                   ESP32-S3 firmware
    main/
      main.c                  app_main + the tank task (0: reflex+render, 1: LLM)
      display_port.h          + display_port_sh8601.c / display_port_stub.c
      touch_port.h            + touch_port_ft3168.c   / touch_port_stub.c
      audio_port.h            + audio_port_es8311.c   / audio_port_stub.c
      battery_port.h          + battery_port_axp2101.c
      imu_port.h              + imu_port_qmi8658.c
      codec_port.h            + codec_port.c
      brightness.c  board_pins.h  net_port_esp.c  director.c  update_mode.c ...
    components/
      tankcore/               common/*.c + persist_port_esp.c + rtc_port_pcf85063.c
      llm/                    the on-device LLM kernel
    partitions.csv            nvs + ota_0 + model(8MB) + otadata + ota_1 + storage
    sdkconfig.defaults        octal PSRAM, 16MB, 240MHz, Wi-Fi, no BT
    sdkconfig.round / sdkconfig.watch   per-board overlays
  model/out/model_q4.bin      ~7.5 MB, flashed to the `model` partition
  assets/sounds/sounds.bin    embedded;  model/out/tokenizer.bin  embedded
```

**Key fact: the device build does NOT use LVGL.** The game renders itself into an
RGB565 buffer with `common/render.c`; the display port just ships that buffer to the
panel. (LVGL only exists in `sim/` for the desktop simulator.) This removes the single
biggest source of conflict with the vendor demo.

The render loop (`main.c`, tank task) is roughly:

```
poll imu/touch/keys -> tank_tick -> render_tank(&tank, fb[cur], TANK_W)
   -> overlay page/card/notice -> display_port_flush(fb[cur]) -> vTaskDelay(...)
```

`display_port_flush()` receives a **full `TANK_W x TANK_H` (448x368) frame** and may
return before the DMA is done. `display_port_panel_to_tank(px,py)` is the inverse
mapping the touch port uses.

---

## 2. Compatibility matrix

| Feature | pocket-tank today | Our board | Verdict |
|---|---|---|---|
| SoC / RAM / flash | ESP32-S3R8, 8 MB octal PSRAM, 16 MB | ESP32-S3R8, 8 MB octal PSRAM, 16 MB | **identical** |
| CPU | 240 MHz | 240 MHz | identical |
| Display | SH8601/CO5300 QSPI AMOLED 368x448 | **ST7789T SPI 240x320**, SPI3 @80 MHz | **new display port** |
| Touch | FT3168 / CST816 / CST9217 (I2C) | **CST328 @0x1A** (CST3530 @0x58 fallback), I2C port 1 | **new touch port** |
| Audio | ES8311 codec + NS4150B amp (I2S + I2C) | **PCM5101 I2S DAC** (no I2C, no amp ctrl) | **new audio port** |
| IMU | QMI8658 @0x6B/0x6A | **QMI8658 @0x6B** | **reuse** (+ axis calibration) |
| RTC | PCF85063 @0x51 | **PCF85063 @0x51** | **reuse** |
| Battery | AXP2101 PMIC fuel gauge | **ADC1_CH7 / GPIO8** divider 3:1 | **new battery port** |
| Power key / latch | AXP2101 PWRON | **GPIO6 key + GPIO7 power-hold latch** | **new battery/key** |
| IO expander | TCA9554 on the 1.8 | none | `board_has_expander()=false` |
| Storage | SPIFFS `storage` partition (OTA) | none used | keep partition table |
| Screen world | 448x368 landscape (default) | 448x368 landscape, downscaled | **display port handles scaling** |

---

## 3. Our board's pin map (what the port must drive)

| Function | Pins | Used by |
|---|---|---|
| LCD SPI3 | SCLK 40, MOSI 45, CS 42, DC 41, RST 39, BL **5** (LEDC) | `display_port_st7789.c` |
| Main I2C (port 0) | SCL 10, SDA 11 — RTC 0x51, IMU 0x6B | `display_port_init`, IMU, RTC |
| Touch I2C (port 1) | SCL 3, SDA 1, INT 4, RST 2 — CST328 0x1A | `touch_port_cst328.c` |
| I2S (PCM5101) | BCLK 48, LRCK/WS 38, DOUT 47, no MCLK | `audio_port_pcm5101.c` |
| Battery ADC | ADC1_CH7 (GPIO8), divider x3, `Measurement_offset` 0.9945 | `battery_port_adc.c` |
| Power | KEY GPIO6 (input), POWER_HOLD GPIO7 (output high latches) | `battery_port_adc.c` + early init |

Free GPIOs if ever needed: **9, 12, 13, 15, 18, 21**. Off-limits: 26-37
(flash/octal PSRAM), 19/20 (USB), 43/44 (UART0), strapping 0/3/45/46.

---

## 4. Where the port lives, and the build wiring

Build **inside `pocket-tank-main/firmware/`**. Do not try to graft pocket-tank onto the
vendor `main/` — the vendor BSP and pocket-tank are two complete, independent driver
stacks; merging them invites duplicate symbols and I2C-driver conflicts.

### 4.1 New files

| File | Purpose |
|---|---|
| `firmware/main/display_port_st7789.c` | the display seam + board identity + `board_i2c_bus()` + power latch |
| `firmware/main/touch_port_cst328.c` | the touch seam (CST328) |
| `firmware/main/audio_port_pcm5101.c` | the audio seam |
| `firmware/main/battery_port_adc.c` | the battery + PWR-key seam |
| `firmware/main/codec_port_stub.c` | `codec_port.h` no-ops for a board with no codec |
| `firmware/main/Vernon_ST7789T.c` / `.h` | **copied from `main/LCD_Driver/Vernon_ST7789T/`** — the vendor-verified panel driver |
| `firmware/sdkconfig.lcd28` | board overlay: `CONFIG_POCKET_TANK_LCD28=y` etc. |

### 4.2 Files to edit

| File | Change |
|---|---|
| `firmware/main/board_pins.h` | add a `TANK_LCD28` pin section (display/bus/power) |
| `firmware/main/Kconfig.projbuild` | add `POCKET_TANK_LCD28`, `POCKET_TANK_DISPLAY_ST7789`, `POCKET_TANK_TOUCH_CST328`, `POCKET_TANK_AUDIO_PCM5101` |
| `firmware/main/CMakeLists.txt` | select the new sources per config; add `Vernon_ST7789T.c` to `SRCS` |
| `firmware/components/tankcore/CMakeLists.txt` | `if(CONFIG_POCKET_TANK_LCD28) target_compile_definitions(... TANK_LCD28)` |
| `common/version.h` | add the `lcd28` board id under `#if defined(TANK_LCD28)` |
| `firmware/main/idf_component.yml` | drop `esp_lcd_sh8601`, `esp_lcd_touch_ft5x06`, `esp_lcd_touch_cst816s` (unused); `esp_lcd` is built-in |
| `firmware/main/main.c` | **one line**: call `board_early_init()` first (power latch), see 8.3 |

Do **not** add `TANK_LCD28` to `common/tank.h` — it must not change `TANK_W/TANK_H`; the
board keeps the default **448x368 landscape world** and the display port scales it.

### 4.3 Partition table and assets

`firmware/partitions.csv` already fits 16 MB (`ota_0` 2.5 MB, `model` 8 MB, `ota_1`
2.5 MB, `storage` 2.9 MB). Keep it. Flash:

```
idf.py -B build-lcd28 -DSDKCONFIG=build-lcd28/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lcd28" build
idf.py -B build-lcd28 -p COMx flash
python -m esptool --chip esp32s3 -p COMx -b 460800 write_flash 0x290000 ../model/out/model_q4.bin
```

(The project's `tools/flash.sh` is macOS-only; on Windows use the commands above, or
add a small `.bat`/PowerShell equivalent. Signing/OTA is optional for local bring-up.)

---

## 5. The display port (the crux)

### 5.1 Geometry

- World: **448x368** (`TANK_W`x`TANK_H`), unchanged.
- Panel native: **240x320 portrait**. Present **landscape 320x240** so no software
  transpose is needed: `esp_lcd_panel_swap_xy(panel, true)` + the right
  `esp_lcd_panel_mirror(...)` (that bit is calibrated on the bench; the vendor's
  LVGL `ROT_90` path uses `mirror(true,true)`).
- Scale: **FIT** (uniform, aspect-preserving) = `min(320/448, 240/368) = 0.652`
  => a **292x240** image, centred, with **14 px black bars** left/right.
  (Simpler alternative: non-uniform stretch to the full 320x240 — no bars, ~9.5 %
  horizontal distortion. Recommend FIT first.)

Downscale math (area-filtered; each dest pixel averages the source box):

```
dst[dy][dx] = box_avg( src[ sy0..sy1 ][ sx0..sx1 ] ),  sx = dx*448/292, sy = dy*368/240
write __builtin_bswap16(dst)   /* ST7789 wants big-endian RGB565; tank fb is LE */
```

### 5.2 Init

```c
bool display_port_init(void) {
    board_power_hold();                       /* GPIO7 high - see 8.3 */
    ledc_timer_config(... 13-bit, 5kHz ...); ledc_channel_config(GPIO5 ...);
    spi_bus_initialize(SPI3_HOST, &{ .sclk=40, .mosi=45, .max_transfer_sz=... }, SPI_DMA_CH_AUTO);
    esp_lcd_new_panel_io_spi(SPI3_HOST, &{ .dc=41,.cs=42,.pclk_hz=80M,.trans_queue_depth=10,
                                            .on_color_trans_done=on_flush_done }, &io);
    esp_lcd_new_panel_st7789t(io, &{ .reset_gpio_num=39,
                                     .rgb_endian=LCD_RGB_ENDIAN_BGR, .bits_per_pixel=16 }, &panel);
    esp_lcd_panel_reset(panel); esp_lcd_panel_init(panel);
    esp_lcd_panel_swap_xy(panel, true); esp_lcd_panel_mirror(panel, CAL_MX, CAL_MY);
    esp_lcd_panel_disp_on_off(panel, true);
    set_backlight(BRIGHT_DEFAULT);
    /* board identity: round=watch=expander=false, pwr_sense=-1, view=FULL */
    s_i2c = i2c_new_master_bus(port 0, SCL 10, SDA 11);   /* RTC + IMU */
    return true;
}
i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c; }   /* touch uses its own port-1 bus */
```

Reusing the vendor `Vernon_ST7789T` panel driver means the ST7789T register table is
exactly the one proven on this glass (the generic `esp_lcd_new_panel_st7789` is a
fallback if the copy won't build).

### 5.3 Flush (striped, DMA-friendly)

```c
void display_port_flush(const uint16_t *fb) {
    for (int y = 0; y < 240; y += STRIPE) {
        int h = min(STRIPE, 240 - y);
        downscale_rows(fb, y, h, s_stripe);            /* box filter + bswap into internal DMA RAM */
        esp_lcd_panel_draw_bitmap(panel, BAR_X, y, BAR_X+292, y+h, s_stripe);
        xSemaphoreTake(done, portMAX_DELAY);           /* return only once the wire has the stripe */
    }
}
```

Stripe buffer size `292 x STRIPE x 2` lives in **internal RAM** (e.g. 292x16x2 = 9.3 KB)
so it is always DMA-capable; the full 448x368 frame stays in PSRAM where the game
allocates it. `display_port_flush_prof()` reports the wait/send times from the
semaphore like the AMOLED port does.

### 5.4 The rest of the display contract

- `display_port_set_inverted(bool)` — the IMU 180° flip. Either flip `mirror` on the
  panel, or flip `dy`/`dx` in the downscale. Keep the touch inverse in sync.
- `display_port_set_brightness(0..255)` / `display_port_brightness()` — LEDC duty
  (map to 0..100 % using the vendor's `Duty = MAX - 81*(100-Light)` curve); keep across
  `wake()`.
- `sleep()/wake()` — panel sleep-in / backlight off, and re-init/backlight on.
- `display_port_panel_to_tank(px,py,&tx,&ty)` — invert the presentation:
  `tx = (px - 14) / 0.652`, `ty = py / 0.652` (`(0,0)` if outside).
- `frame_origin` = (0,0); `set_view/view` = FULL; `board_is_round/watch/expander` =
  false; `board_pwr_sense_pin` = -1; `deep_sleep_pins/bus/deep_standby` = no-ops.

### 5.5 Performance

Measured on the AMOLED: render ~36 ms/frame. Add the box-filter downscale (~2-4 ms) and
a ~14 ms wire time at 80 MHz => roughly **45-55 ms/frame (~19-22 fps)**. The tank's own
floor is ~16.6 fps, so this is acceptable. If it is too slow, the easy levers are:
nearest-neighbour downscale, a larger flush stripe, or slightly larger frame step.
(The `async_memcpy` scene-prefetch installs *after* the display grabs its DMA channel —
main.c already guards this — but verify it does not disturb SPI3.)

---

## 6. The touch port (CST328)

Clone `touch_port_ft3168.c` — it contains **all** the UI hit-testing (tap/hold/drag
gestures, stats card, milestones/settings/shop/updates pages, battery pill, reset
prompt). Replace only the chip layer:

- **Bus**: the touch is on **I2C port 1** (SCL 3, SDA 1, RST 2, INT 4), separate from
  RTC/IMU on port 0. Create this bus inside the touch port (it owns port 1 exclusively).
- **Chip init**: probe CST328 @0x1A, fall back to CST3530 @0x58. Reuse the vendor's
  register protocol from `main/Touch_Driver/CST328.c`: reset, read count register,
  read the packed 5-point X/Y block, clear. Either port that read onto `i2c_master`
  (preferred, matches the rest of pocket-tank) or reuse the vendored `esp_lcd_touch`
  CST328 driver on this dedicated bus.
- **Coordinates**: this is the part to verify on the bench. The vendor config is
  peculiar: `x_max = LCD_V_RES (320)`, `y_max = LCD_H_RES (240)`, `swap_xy = 0`, while
  the LCD is 240x320 portrait — i.e. the digitiser's raw axes appear **90°-rotated**
  relative to the LCD. Start with a raw->panel swap (`panel_x = raw_y`,
  `panel_y = raw_x`), then `display_port_panel_to_tank()`, and calibrate the offset/gain
  constants the way `touch_port_ft3168.c` already does (`CAL_*`).
- **Inversion**: `touch_port_set_inverted()` must mirror the mapped tank coords when the
  IMU flips the screen.
- **Snapshot / gesture timing**: keep the FT3168 port's timings (tap <350 ms & <24 px,
  hold >300 ms) — they are board-independent; only the raw scale changes.

---

## 7. The audio port (PCM5101)

Clone `audio_port_es8311.c`, strip the codec/amp/rail, change the pins:

```c
#define PIN_I2S_BCLK 48
#define PIN_I2S_WS   38
#define PIN_I2S_DOUT 47
/* no MCLK (vendor uses NC), no amp GPIO, no codec I2C, no ALDO rail */
i2s_std_config_t std = {
  .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SND_RATE),                       /* 16 kHz */
  .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
  .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk=48, .ws=38, .dout=47, .din=I2S_GPIO_UNUSED },
};
```

- `audio_port_init()` returns true on a valid sound bank (no codec probe). `common/audio.c`
  mixes **mono s16** at `SND_RATE`; feed `audio_render()` blocks to `i2s_channel_write()`
  from a core-1 task, with the same mutex.
- `bring_up()` = enable I2S + write ~20 ms of silence (the PCM5101's own pop suppression);
  no codec settle. `bring_down()` = write silence, disable I2S.
- `audio_port_sleep()` stops I2S; `audio_port_deep_sleep_pins()` drives BCLK/WS/DOUT low
  and holds them (avoids leakage into the always-powered DAC).
- `codec_port.h` gets a **stub** (`present=false`, all no-ops) so `main.c`'s calls are
  harmless; `audio_port_pcm5101.c` must not call the codec.

---

## 8. The battery + power-key port (no PMIC)

Clone the shape of `battery_port_axp2101.c`, replace the AXP2101 reads with the vendor
`BAT_Driver`:

- `battery_port_init()` — `BAT_Init()` (ADC1_CH7 + eFuse calibration); return true.
- `battery_port_read(frac,charging)` — `BAT_Get_Volts()` returns
  `(mV*3.0/1000)/0.9945` (divider 3:1). Convert to a fraction with a Li-ion
  discharge curve; `charging=false` (no charger-detect line in the vendor driver).
  Cache ~1 s.
- `battery_port_vbat_mv()` — `BAT_Get_Volts()*1000`.
- `battery_port_state()` — `BAT_ON_BATTERY` (no cable detection).
- `battery_port_dump()` — log the ADC voltage.
- `set_rail()/trim_rails()/pin_rail()` — no-ops (no PMIC; `board_has_expander()==false`
  makes `main.c` call the first two — they must exist and do nothing).

### 8.1 The PWR key (`battery_port_key_*`)

`main.c` calls `battery_port_key_init()` and, every frame, `battery_port_key_poll()`
(0 = nothing, 1 = short press, 2 = long press). Our key is **GPIO6** (active low).
Implement a small press-time state machine mirroring the vendor thresholds
(`PWR_Key.h`: sleep 10, restart 15, shutdown 20 * PWR_Loop ticks*). The key drives the
manual sleep path and the night power decisions.

### 8.2 "Power off"

`battery_port_poweroff()` can reproduce the vendor's `Shutdown()`: drive **GPIO7 low**
to release the power latch (the board then powers off; a PWR-key press reboots it).
If the deep-sleep path is preferred, return `false` and let the tank deep-sleep instead.

### 8.3 The power latch (must be the very first thing)

The board is held on by **GPIO7 high** (vendor `PWR_Init`: configure GPIO6 input, GPIO7
output; set GPIO7 high while the PWR key is down). On battery, if GPIO7 is not latched
promptly the board can cut out. `pocket-tank`'s `app_main` does `nvs_start()`,
`director_early()`, `batlog_init()`, `assert_plan()` *before* `display_port_init()`, so
the latch cannot live there. Add a board hook and call it as the **first line of
`app_main`**:

```c
void board_early_init(void) {                 /* in display_port_st7789.c */
    gpio_config(GPIO6 input); gpio_config(GPIO7 output);
    gpio_set_level(GPIO7, 1); gpio_hold_en(GPIO7);   /* latch on, survive deep sleep */
}
```

(Take `gpio_hold_dis(GPIO7)` once the key/sleep code is up, so `battery_port_poweroff()`
can still drop it.) **Delivered in `battery_port_adc.c`** (not the display port — the
battery file is always compiled for LCD28, so `board_early_init()` can't dangle for a
stub-display build).

---

## 9. Reused ports: IMU and RTC (same chips)

- **`imu_port_qmi8658.c`** — QMI8658 @0x6B on `board_i2c_bus()`. Works as-is; only the
  **up-axis/sign** is board-specific (`s_up_axis`, `s_up_sign`). Boot log prints
  `imu: g=[x y z]`; hold the board upright, see which axis carries ~1 g, set the two
  constants (or add a `board_is_lcd28()` branch next to the round/watch ones).
- **`rtc_port_pcf85063.c`** — PCF85063 @0x51 on `board_i2c_bus()`. Works unchanged; it
  probes, seeds from build time if unset. Gives the tank a real wall clock from boot.
- **`codec_port`** — stub (see 7).

---

## 10. Board identity and Kconfig

```c
/* common/version.h */
#elif defined(TANK_LCD28)
#define PT_BOARD      "lcd28"
#define PT_BOARD_NAME "ESP32-S3-Touch-LCD-2.8"
```

```kconfig
# firmware/main/Kconfig.projbuild
config POCKET_TANK_LCD28
    bool "The ESP32-S3-Touch-LCD-2.8-V2 (ST7789T + CST328)"
    default n
config POCKET_TANK_DISPLAY_ST7789
    bool "Drive the ST7789T panel over SPI3"
    default n
config POCKET_TANK_TOUCH_CST328
    bool "Read the CST328 capacitive touch"
    default n
config POCKET_TANK_AUDIO_PCM5101
    bool "Play the sound bank through the PCM5101 I2S DAC"
    default n
```

```cmake
# firmware/main/CMakeLists.txt
if(CONFIG_POCKET_TANK_DISPLAY_ST7789)
    list(APPEND SRCS display_port_st7789.c Vernon_ST7789T.c)
elseif(CONFIG_POCKET_TANK_DISPLAY_SH8601)
    list(APPEND SRCS display_port_sh8601.c)
else()
    list(APPEND SRCS display_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_TOUCH_CST328)
    list(APPEND SRCS touch_port_cst328.c)
elseif(CONFIG_POCKET_TANK_TOUCH_FT3168)
    list(APPEND SRCS touch_port_ft3168.c)
else()
    list(APPEND SRCS touch_port_stub.c)
endif()
if(CONFIG_POCKET_TANK_AUDIO_PCM5101)
    list(APPEND SRCS audio_port_pcm5101.c)
elseif(CONFIG_POCKET_TANK_AUDIO_ES8311)
    list(APPEND SRCS audio_port_es8311.c)
else()
    list(APPEND SRCS audio_port_stub.c)
endif()
list(APPEND SRCS battery_port_adc.c codec_port_stub.c)   # LCD28 battery/codec
```

`TANK_LCD28` is added as a compile definition in
`firmware/components/tankcore/CMakeLists.txt`. OTA will look for `latest-lcd28.json`;
unless you host one, the updates page simply finds nothing — harmless.

---

## 11. Implementation order (milestones with pass signals)

| # | Step | Pass signal |
|---|---|---|
| **M0** ✅ | Add `TANK_LCD28` + Kconfig + CMake; build with **stub** display/touch/audio and the ADC battery. Flash app + model. **Done 2026-10-09.** | Log: `PSRAM plan OK`, `model partition … mmap'd`, tank task starts, frames counted (`display` stub), no panic — **all seen; 24.5 fps at the stub**, 4.11 V from the ADC. |
| **M1** ✅ | `display_port_st7789.c` (init + downscale flush + brightness). Calibrate MADCTL mirror + byte order. **Done 2026-10-09.** | The fish tank appears on the 2.8" panel, correct orientation/colours, ~15-25 fps in the log. **Seen: `panel up: 320x240 landscape … FIT (292x240)`, 24.3 fps, frame 41.2 ms, flush fill 8.4 ms + wire 7.4 ms, zero `draw_bitmap` errors. Bench eye-checks 2026-10-09: 0x68 flipped top/bottom, 0xE8 flipped both axes → final **MADCTL 0x28** (`swap_xy` + `mirror(false,false)`, the 180° of the vendor's ROT_90) verified upright; colours: the welcome's palette is channel-invariant enough to fool the first pass — a bar test proved this glass has **reversed inversion polarity (0x20 inverts, 0x21 is normal)** and wants a **native (unswapped) RGB565 wire**; fixed and re-verified (R/G/B/W/teal bars true, water teal-blue). **Shipped view flipped to stretch 2026-10-09** — the tank fills the whole 320 px (no 14 px bars; fish ~9.6% longer) with touch following via the shared `display_port_panel_to_tank` gains (same window constants) — user-verified on the glass. |
| **M2** ✅ | `touch_port_cst328.c` (bus, chip read, mapping, gestures). | Taps select fish / feed / flip light; holds draw fish; pages (shop/settings/milestones) respond; drags wipe algae. **Done 2026-10-09.** Calibrated (two passes of nine crosses via `calib28.py`): the panel's chip probes as **CST3530 @0x58** (no CST328 @0x1A — vendor demo would fall back the same way); mapping = `cst_to_panel` mirror (short axis runs bottom-up) + CAL `x gain 0.9867 off -13.5, y gain 1.0249 off +14.3`. **Ack bug found by the stroke log:** `cst3530_end()` wrote **00 D0 02 AB** instead of **D0 00 02 AB** (first two bytes swapped), so the chip never saw its report ack at `0xD00002AB` and kept serving the same "finger down" block for **1.9-10 s** — every tap classified as a hold; fish taps / double-taps / feeds / swipes could never fire. Fixed the byte order; re-verified on the glass — **all touch features work (fish cards, double-taps, feed, wipe algae).** |
| **M3** ✅ | `battery_port_adc.c` (meter + key). | Battery pill shows a sane %; PWR key short-press brings the sleep prompt/flow; power latch holds on battery. **Done 2026-10-09.** ADC1_CH7 (GPIO8, 3:1 divider, eFuse calib) reports ~4110 mV / ~92% in the tank log. PWR key BOTH ends verified on the bench: `keytime` trace logged presses at 195 / 210 / 175 ms, and live — a short press runs the sleep flow (screen off), a press during the grace wakes in place (the boot log's `t=` reset was that first press sleeping the board). Hold latch GPIO7 latched in `board_early_init`; `poweroff` drops it. Deep-sleep wake is the PWR key as ext1 (M6 wired GPIO6; BOOT stays as a backup). |
| **M4** ✅ | `audio_port_pcm5101.c` + codec stub. | Feeding a fish / tapping the glass plays a cue; goes silent after idle. **Done 2026-10-09.** Cloned the ES8311 port, stripped codec/amp/rail: BCLK 48 / WS 38 / DOUT 47, **no MCLK** (vendor wires NC; the PCM5101 auto-detects its clock from BCK/LCK — exactly `BSP_I2S_GPIO_CFG` in the vendor `PCM5101.h`), Philips 16-bit mono, 20 ms zero-settle pop suppression, 3 quiet pins (BCLK/WS/DOUT) for deep sleep. Log cycle proved live: `audio: up in 19 ms (DAC, no codec/amp)` … `down (idle)`. `snd list` shows all 24 cues (deferred ones are context-gated, as designed); played `card_open feed eat confirm` back-to-back, each cycling up/down cleanly — **user confirmed audibly ("yes able to hear").** |
| **M5** ✅ | IMU axis calibration + RTC (verify). | Screen auto-flips when you turn the board over; `clock` log shows a real time from the RTC. **Done 2026-10-09.** IMU axis **verified by bench measurement — zero code change**: a 60 s `imu 40` trace caught the board held upright (`g ≈ [-985, -15485, +65]`, the ~1 g on **Y negative**) and flat on the bench (`Z ≈ -16600`), which matches the port's existing default `s_up_axis=1, s_up_sign=-1` ("axis -Y") — no `board_is_lcd28()` branch needed. Flip chain confirmed wired end-to-end: `imu_port_poll` → `tank_orient` (unlocked) → `display_port_set_inverted` (FIT fill rotates 180° in `fill_row`) + `touch_port_set_inverted` (mirrors `tx/ty` in tank space); flat-on-bench guard keeps it from flapping on the table. **User confirmed on the glass: the whole image rotates 180° when the board is turned over and tapping a fish still works while inverted (touch mirrors in sync).** RTC verified: `clock` logs `now 1791515900 | RTC chip yes` (a real wall clock, ~2026-10-09), boot log `rtc: system clock set from RTC: 2026-10-09 03:05`. |
| **M6** ✅ | Performance + sleep + OTA polish. | Stable deep sleep on battery; optional signed OTA to a `lcd28` manifest. **Done 2026-10-09.** Sleep: the PWR key (GPIO6, active low) is now the **deep-sleep ext1 wake** — `board_key_wake_pin()` (new seam next to `board_pwr_sense_pin`: LCD28 → `L28_PIN_KEY`, AMOLED/stub → -1) and a `main.c` `deep_sleep_now()` branch that arms `esp_sleep_enable_ext1_wakeup(1ULL<<6, ANY_LOW)` (RTC-peripherals-down) with BOOT (GPIO0) still armed as an ext0 backup. Bench-proven live: `deepsleep 120` → 5 s grace → deep sleep (USB-Serial/JTAG vanished with it) → **PWR key press → boot logs `wake from deep sleep (the PWR key): … battery 90% 4112 mV`, `wake: 2 of 2 fish put back where they were`** — the tank resumed at 25.5 fps, fish and clock intact; the user confirmed ("pressed … it woke up"). The wake press is eaten by the existing 3 s `KEY_BOOT_DEAF_US` window so it can't re-sleep the tank. Performance: 25.5-25.6 fps / frame 39 ms (better than M1's 24.3) — well above the 19 fps floor, no work needed. OTA: **skipped as planned** — the signing key is not on this machine (unsigned cable builds only, §11.1). |

---

## 12. Risks and things to verify on the bench

1. **Touch axes** — the vendor CST328 config suggests the digitiser is 90°-rotated vs
   the LCD. Budget time for calibration; the `touch_port_ft3168.c` `CAL_*` pattern is
   built for exactly this.
2. **Panel init/gamma** — mitigated by copying `Vernon_ST7789T` rather than using the
   generic driver. If colours are swapped, toggle the panel BGR/`MADCTL` bit and/or the
   `bswap16` in the downscale.
3. **Power latch** — if `GPIO7` is not latched before `nvs_start()`, a battery-powered
   board may die at boot. `board_early_init()` first line of `app_main`.
4. **DMA sharing** — `main.c` installs `esp_async_memcpy` (scene prefetch) after the
   display takes its SPI DMA channel; on SPI3 this is fine in theory but verify the
   panel is not glitched with prefetch on (disable the prefetch if it is).
5. **Frame budget** — 448x368 render + downscale. If fps is too low, prefer FIT ->
   nearest-neighbour, or accept ~19 fps.
6. **IMU orientation** — needs the same one-time axis/sign calibration as every board.
7. **Audio pop/latency** — the PCM5101 is always powered; tune the silence-settle and
   the idle bring-down.
8. **OTA** — only if you want it; otherwise leave the updates page finding nothing.
9. **Build host** — the project's tooling is macOS/bash; on Windows use `idf.py` +
   `esptool` directly (section 4.3). Model partition offset is `0x290000`.
10. **GCC 13.2 quirks** (dealt with in M0, see §11.1) — the Xtensa backend ICEs
    `q4_model.c` at `-ffast-math` (workaround `-fno-finite-math-only`), and
    `-Werror=format-truncation` caught an upstream buffer bug at `main.c:397`.

---

## 11.1 M0 delivered — what changed and the bench notes

Shipped (all inside `pocket-tank-main`):

- **`firmware/main/battery_port_adc.c`** — the ADC gauge (ADC1_CH7, `adc_oneshot` +
  eFuse calibration, divider 3:1 and the vendor `0.9945` offset corrected, Li-ion
  voltage→% curve), the GPIO6 PWR-key state machine (30 ms debounce, 1.5 s long press)
  and `board_early_init()` (GPIO7 hold latch, `gpio_hold_en`).
- **`firmware/main/codec_port_stub.c`** — no codec on this board: the `codec_port_*`
  API as no-ops.
- **`firmware/sdkconfig.lcd28`** — the board overlay: `POCKET_TANK_LCD28=y`, the
  M1/M2/M4 seam switches off, and **signing disabled** (`firmware/keys/` — the OTA
  signing key — is not on this machine, so a local cable build must be unsigned).
- Edits: `board_pins.h` (LCD28 pin block + `board_early_init()` decl), `Kconfig.projbuild`
  (LCD28 + the future ST7789/CST328/PCM5101 switches), `main/CMakeLists.txt` (LCD28
  sources, `esp_adc` required, future port branches), `tankcore/CMakeLists.txt`
  (`TANK_LCD28`), `common/version.h` (`PT_BOARD "lcd28"`), `main.c` (latch call as the
  first line of `app_main`; `board_i2c_bus()` now external for the ST7789 build too;
  `char why[24]`, see below), `idf_component.yml` (dropped the Waveshare managed
  components — they only serve the AMOLED ports — and relaxed to `idf >= 5.3`).

Two **toolchain (GCC 13.2 / IDF 5.3.5)** workarounds were needed, both commented in the
files:

- **`-ffast-math` ICE** — `common/llm/q4_model.c` trips an Xtensa backend
  "insn does not satisfy its constraints" (`recog.cc:2692`) at `-O3` **and** `-O2`. It
  is armed by `-ffinite-math-only`; `-O2 -ffast-math -fno-finite-math-only` compiles it
  deterministically (verified 3×). Applied in `components/llm/CMakeLists.txt`; revisit
  on a newer toolchain.
- **`-Werror=format-truncation`** — `main.c:397` formatted `"deep %d"` (up to 11 digits)
  into `char why[8]`: a real latent buffer bug. Enlarged to `why[24]`; the 8-byte NVS
  ring field still truncates to what it always would.

Bench evidence (boot log, board on COM3): `pocket-tank v0.3.3 alpha (build 1) for
lcd28` · `battery: power hold latched (GPIO7 high), PWR key on GPIO6` ·
`PSRAM plan OK (need 4228 KB)` · `battery ADC1_CH7 (GPIO8) up | 4118 mV` ·
`model: trailer: model v3m, 7557640 bytes - good` · `model partition 8192 KB mmap'd` ·
`codec: no codec` · `audio: stub port: 24 cues` · tank task up ·
`display: 24.5 fps` (frame 40.8 ms) · advisor 10.6 tok/s · no panic.

Flash recipe (Windows, from `pocket-tank-main/firmware`):

```
idf.py -B build-lcd28 -DSDKCONFIG=build-lcd28/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lcd28" build
idf.py -B build-lcd28 -p COM3 flash
python -m esptool --chip esp32s3 -p COM3 -b 460800 write_flash 0x290000 ../model/out/model_q4.bin
```

---

## 11.2 M1 delivered — what changed and the bench notes

Shipped (all inside `pocket-tank-main`):

- **`main/Vernon_ST7789T/`** — the vendor's ST7789T panel driver, **copied unmodified**
  (`Vernon_ST7789T.c/.h`, from this repo's `main/LCD_Driver/Vernon_ST7789T/`): the
  vendor init table (SLPOUT, COLMOD 0x55, gamma, INVON, DISPON), which no generic
  `esp_lcd_new_panel_st7789` replicates. `CMakeLists.txt` compiles it from its own
  folder and `INCLUDE_DIRS "."` resolves its `"Vernon_ST7789T/Vernon_ST7789T.h"`.
- **`main/display_port_st7789.c`** — the LCD28 display port: SPI3 @80 MHz
  (SCLK40/MOSI45/DC41/CS42/RST39), panel IO queue depth 10, `board_i2c_bus()` (port 0,
  SCL10/SDA11 — the RTC + IMU live on it), LEDC backlight on GPIO5 (13-bit 5 kHz,
  `0..255` → the vendor's `0..100` duty), and the full-width (stretch) flush pipeline
  (see the last bench note). Orientation after the vendor init (whose MADCTL is
  hardcoded 0x00) is set by `swap_xy(true) + mirror(false,false)`, which also lands
  the dev-config BGR bit: MADCTL 0x28 (the 180° of the vendor's ROT_90; bench rejects
  along the way: `mirror(true,false)`/0x68 flipped top/bottom, `mirror(true,true)`/0xE8
  flipped both axes).
- **Full-width window (stretch, shipped 2026-10-09)** — the 448x368 frame becomes
  320x240 (half-pixel-centre nearest-neighbour, precomputed row/col maps; the window
  is `WIN_W = 320` across `WIN_X = 0` — the earlier FIT was 292x240 with 14 px black
  side bars); 180° flip (`display_port_set_inverted`) in the same fill; the wire is
  native RGB565 words, low byte first (no swap — this glass's corrected polarity, see
  the M1 row). Ping-pong DMA stripes of 32 rows (2x20 KB), `display_port_flush_prof`
  accounting wait-vs-send.
- **`main/CMakeLists.txt`** — ST7789 branch sources + `esp_driver_ledc` required.
- **`firmware/sdkconfig.lcd28`** — `CONFIG_POCKET_TANK_DISPLAY_ST7789=y` (the M0
  generated `sdkconfig` is sticky: rebuild the config by deleting `build-lcd28/sdkconfig`
  once after flipping a seam default).

Bench evidence (boot log, board on COM3, app `0x14dda0` @ 0x10000): `st7789: board:
LCD28 … the main I2C bus (SCL10, SDA11) is this port's` · `panel up: 320x240 landscape,
the 448x368 tank frame at FIT (292x240, the bars at x 0..13)` ·
`rtc: system clock set from RTC: 2026-10-09 03:05` (the RTC answered — dead under the
M0 stub's NULL bus) · `imu: QMI8658 up at 0x6b: orientation axis -Y` ·
`display: 24.3 fps | render 20.8 ms flush 15.8 ms` ·
`frame 41.2 ms = … render 20.8 + flush 15.8 … | flush: fill 8.4, wire wait 0.0, send
calls 7.4` · no `draw_bitmap` errors, no panic. Wire time at 80 MHz is ~7.4 ms of the
frame; the fill (8.4 ms) is the downscale — FPS is render-bound as planned.

Bench notes:

- **Orientation/colours** — MADCTL 0x28 (swap + BGR, no mirrors) is chosen to present
  the frame upright. Bench reads (2026-10-09): 0x68 (swap+MX) flipped top/bottom;
  0xE8 (swap+MX+MY) 180° — the same relation as the vendor's LVGL ROT_90/ROT_270
  pair, so flipping both `mirror` axes off was the fix (`panel_upright()`); a
  left/right-only flip would be `mirror(false,true)` (0xA8). Colours: the welcome
  page's palette reads true — NO red/blue swap — so `rgb_endian` stays BGR.
- **The colour night (fixed 2026-10-09, after M1 shipped)** — the welcome page is
  mostly dark panel + white text, so M1's "colours verified" was channel-invariant
  and unreliable. A bar test (R/G/B/W/teal) proved two things on the glass: (1) the
  wire must be **native RGB565** — the tank's words go out verbatim, low byte first,
  no `be565` byte swap (the panel reads exactly what the bytes say); (2) this glass
  has **REVERSED inversion polarity**: sending the datasheet's `0x20` (INVOFF)
  INVERTS every colour (the white bar read black, water read white), and `0x21`
  (INVON) is the normal state. The port therefore calls
  `esp_lcd_panel_invert_color(true)` at init and wake (`0x21` = normal here) and
  sends the frame unswapped. With that, R/G/B/W/teal bars read true and the tank is
  back to teal-blue water with coral/kelp fry.
- **The fresh-tank welcome** — a new tank opens the first-run setup (welcome → place
  the bubbles → name/colours); the orange/green shapes around the panel are the two
  newborn fry (`0xff725c` coral + `0x78d67d` kelp). It blocks the screen only
  because stepping past it needs a tap (M2 touch).
- **The touch ack bug (fixed 2026-10-09, found by the stroke log)** — M2's first
  bench pass said touch "worked" (NEXT advanced), but on the real tank every tap
  failed to open a fish card. The stroke log (`touch log on`) showed the cause:
  EVERY press read as **1.9-10 s held** with a frozen point (`travel 0`), so no press
  ever met the tap rule (< 350 ms) — fish taps, double-taps, feeds and swipes were
  all structurally impossible. The culprit was the CST3530 END write in
  `cst3530_end()`: it sent **`00 D0 02 AB`** where every other 4-byte register in the
  port goes big-endian and the vendor (`CST3530.c` `END_READ_REG = 0xD00002AB`)
  sends **`D0 00 02 AB`** — two bytes swapped, so the chip never received the
  end-of-report ack and kept re-serving the same "finger down" block for seconds.
  Fix: correct byte order; re-verified on the glass — all touch features work.
- **The IMU/RTC coming up is a bonus of the real bus**, but it also means
  `board_is_*`/`board_pwr_sense_pin()` now answer LCD28-truthfully: no expander, no
  PWR-sense line. **M6 closed that gap**: `board_key_wake_pin()` (LCD28 → GPIO6) makes
  the PWR key the deep-sleep ext1 wake (ANY_LOW), BOOT kept as ext0 backup — bench:
  `deepsleep 120` slept (USB port vanished), a PWR-key press booted with
  `wake from deep sleep (the PWR key): … | battery 90% 4112 mV` + `wake: 2 of 2 fish
  put back where they were`; the tank resumed at 25.5 fps. The wake press is inert
  (`KEY_BOOT_DEAF_US` 3 s). One benign log seen at sleep entry:
  `i2s_common: i2s_channel_disable: the channel has not been enabled yet` (audio was
  already idle-down when `audio_port_sleep()` re-disabled — a no-op double call).
- **Stretch-to-fill is the shipped view (2026-10-09)** — M1 shipped FIT (292x240,
  14 px black bars); an on-glass A/B flipped it to fill the whole 320x240
  (`display_port_st7789.c`: `WIN_W = PANEL_PW`, `WIN_X = 0`, the old `FIT_W/H/X`
  macros renamed `WIN`). The picture runs 9.6% wider, so fish read longer (vertical
  untouched), and touch stayed true because `display_port_panel_to_tank` scales by
  the same window constants (X gain 448/320 instead of 448/292, no bar offset) —
  user-verified both on the glass. Cost: flush 16.6 → 17.8 ms (9.6% more pixels),
  fps still 25.7. Reverting to FIT is two constants (`WIN_W` → 292, `WIN_X` → 14).

---

## 13. Decisions I'd like from you

1. **Scale mode**: FIT (292x240, 14 px black bars, no distortion) — or stretch to fill
   320x240? **Stretch chosen 2026-10-09** after an on-glass A/B (you tried it and kept
   it): the tank fills the whole width (no bars), fish read ~9.6% longer (vertical
   untouched), and touch stayed true — `display_port_panel_to_tank` shares the window's
   X gain, so the mapping switches in the same file/step. In the shipped code the
   window is defined `WIN_W = PANEL_PW`, `WIN_X = 0`; going back to FIT is a
   two-constant revert (`WIN_W` → 292, `WIN_X` → 14). Flush cost rose ~8 % (it pushes
   9.6% more pixels); the tank still ran 25.7 fps.
2. **Audio**: full PCM5101 port now, or stub it for M0-M3 and add later?
   **M0 resolved this for now — the build ships with the audio stub** (M4 adds
   `audio_port_pcm5101.c`); say the word and M4 moves up.
3. **Power off**: implement true power-latch drop, or rely on deep sleep only?
   **Implemented as latch-drop in M0** (`battery_port_poweroff()` drives GPIO7 low,
   matching the vendor's `Shutdown()`). **Bench-verified on a real battery
   2026-10-09**: USB unplugged → the tank kept running on the cell alone (latch
   held); a ~2 s PWR hold ran the power-off — the screen died and the rails truly
   cut (the board was off, not asleep); a PWR tap re-latched the rail and booted
   it on battery alone; after re-plugging USB it was healthy at 25.6 fps. The
   same ride proved the cell charges over USB (92 % → 100 %, 4130 → 4223 mV).
   **Decision: keep latch-drop power-off** — no fallback to deep-sleep-only needed.
4. **Delivery**: do you want me to (a) apply the port directly to a new
   `pocket-tank-main/firmware` build with the new files, and/or (b) append this plan to
   `README.md` as Part E? **M0 is already (a) — applied in place; (b) is optional.**
```

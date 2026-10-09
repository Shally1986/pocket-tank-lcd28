# Pocket Tank on the ESP32-S3-Touch-LCD-2.8-V2 (LCD28)

A port of [pocket-tank](https://github.com/mediacutlet/pocket-tank) (MIT,
© MediaCutlet) to the Waveshare **ESP32-S3-Touch-LCD-2.8-V2** board. This repo
is a fork of upstream `main` with one commit on top: the port.

## What the port changes

The port is deliberately a **thin layer**: upstream's app core (`common/`,
sim, installer, model) is untouched. Hardware access goes through the same
seam files upstream already uses, and all LCD28-specific code lives in the
port seam files:

| Subsystem | LCD28 port file | Board part |
|---|---|---|
| Display | `firmware/main/display_port_st7789.c` + `firmware/main/Vernon_ST7789T/` | ST7789T 320x240, MADCTL 0x28, native RGB565, stretch-to-fill window |
| Touch | `firmware/main/touch_port_cst328.c` | CST3530 @ 0x58, `cst_to_panel` mirror + calibration |
| Audio | `firmware/main/audio_port_pcm5101.c` | PCM5101A I2S, no MCLK, 16 kHz Philips |
| IMU | `firmware/main/imu_port_qmi8658.c` (edited upstream seam) | QMI8658, up-axis −Y |
| RTC | `firmware/main/rtc_port_pcf85063.c` (edited) | PCF85063 |
| Battery / power | `firmware/main/battery_port_adc.c` | ADC meter, GPIO7 power latch, PWR-key (GPIO6) deep-sleep wake |

Other port edits: `firmware/main/main.c` (sleep/wake path, wake-cause logs),
`board_pins.h`, the seam headers, `CMakeLists.txt` / `Kconfig.projbuild`
(seam source selection), `sdkconfig.lcd28`, `partitions.csv`,
`common/version.h` (adds the `TANK_LCD28` board macro). See
`POCKET_TANK_PORT_PLAN.md` for the full milestone-by-milestone account.

## Build & flash

From `firmware/` (ESP-IDF 5.3.x):

```
idf.py -B build-lcd28 -DSDKCONFIG=build-lcd28/sdkconfig "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lcd28" build
idf.py -B build-lcd28 -p COM3 flash
```

Sticky sdkconfig note: delete `build-lcd28/sdkconfig` when flipping a seam
default (it re-generates from `sdkconfig.defaults` + `sdkconfig.lcd28`).

## Keeping up with upstream

The port is a single commit on top of upstream history, so an upstream
release is a normal merge:

```
git fetch upstream
git merge upstream/main
```

Conflicts concentrate in the seam files and `main.c` — the app core merges
cleanly. The whole port is also available as one reviewable diff:
`git diff upstream/main..HEAD` (generated as `port-lcd28.diff` in the port
plan directory at ship time).

## License & attribution

- Upstream pocket-tank: MIT (see `LICENSE`, © MediaCutlet 2026).
- This port's changes: MIT, same terms.
- `firmware/main/Vernon_ST7789T/` is the Waveshare demo's ST7789T driver,
  copied unmodified (protocol reference; courtesy of Waveshare).
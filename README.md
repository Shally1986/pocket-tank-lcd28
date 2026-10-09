# Pocket Tank · ESP32-S3-Touch-LCD-2.8-V2 (LCD28) port

A working port of **[pocket-tank](https://github.com/mediacutlet/pocket-tank)** — the
"pocket fish tank" — to the Waveshare **ESP32-S3-Touch-LCD-2.8-V2** board. The whole
milestone chain (M0–M6 in `POCKET_TANK_PORT_PLAN.md`) is delivered and running on the
glass: colours, touch, battery/PWR key, audible audio, IMU/RTC axes, and PWR-key
deep-sleep wake.

---

## This repository is a fork of pocket-tank

- **Upstream:** <https://github.com/mediacutlet/pocket-tank> (MIT, © MediaCutlet)
- Upstream `main` history is intact here (v0.3.3, tags v0.2.0–v0.3.3 included).
- The port itself is **one commit** on top of upstream `main`, so an upstream release
  is a normal merge — see [Keeping up with mainline](#keeping-up-with-mainline) below.

```text
* 90b107a  port: ESP32-S3-Touch-LCD-2.8-V2 (LCD28): M0-M6, stretch-to-fill, PWR-key deep-sleep wake
* c579605  v0.3.3 (alpha)            <- upstream/main
* 8be5870  v0.3.2 (alpha)
  ...
```

Remotes (`git remote -v`): `origin` → this repo, `upstream` → MediaCutlet/pocket-tank.

The Waveshare demo this port was built against (`main/`, `components/`, vendor drivers)
is **not** included. The one vendored demo file is the panel driver
(`firmware/main/Vernon_ST7789T/`, unmodified, courtesy of Waveshare).

---

## What the port changes

The port is a thin hardware layer. Upstream's app core (`common/`, sim, installer,
model) is untouched; all board-specific code lives in the LCD28 seam files:

| Subsystem | Port file | Board part |
|---|---|---|
| Display | `firmware/main/display_port_st7789.c` + `firmware/main/Vernon_ST7789T/` | ST7789T 320×240 — MADCTL 0x28, native RGB565, inverted polarity, stretch-to-fill window (~25.5 fps) |
| Touch | `firmware/main/touch_port_cst328.c` | CST3530 @ 0x58 — `cst_to_panel` mirror + calibration |
| Audio | `firmware/main/audio_port_pcm5101.c` | PCM5101A I2S — no MCLK, 16 kHz Philips, BCLK48/WS38/DOUT47, ~19 ms cold cycle |
| IMU | reuses upstream `imu_port_qmi8658.c` default | QMI8658 — upright axis −Y (verified on bench) |
| RTC | reuses upstream `rtc_port_pcf85063.c` | PCF85063 — seeds the clock over the display I2C bus |
| Battery / power | `firmware/main/battery_port_adc.c` | ADC meter (~4110 mV ≈ 92%), GPIO7 power latch, PWR key (GPIO6) ext1 deep-sleep wake |

Other port edits: `firmware/main/main.c` (sleep/wake path, wake-cause logs),
`board_pins.h`, the seam headers, `CMakeLists.txt` / `Kconfig.projbuild` (seam source
selection), `sdkconfig.lcd28`, `common/version.h` (adds the `TANK_LCD28` board macro).
A full milestone-by-milestone account lives in `POCKET_TANK_PORT_PLAN.md`.

---

## Build & flash

Requirements: **ESP-IDF 5.3.x** (`C:\esp\v5.3.5\esp-idf` was used), a board on a USB
serial port.

```bat
cd firmware
call <esp-idf>\export.bat          REM one-time per shell, e.g. C:\esp\v5.3.5\esp-idf\export.bat
idf.py -B build-lcd28 -DSDKCONFIG=build-lcd28/sdkconfig "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lcd28" build
idf.py -B build-lcd28 -p COM3 flash   REM adjust -p to your board's port
```

Notes learned on the bench:

- **Sticky sdkconfig** — if you ever flip a seam default in `sdkconfig.defaults` /
  `sdkconfig.lcd28`, delete `build-lcd28/sdkconfig` first; it re-generates on the next
  build.
- **Sleep-grace gotcha** — if the board is in the light-sleep grace period,
  esptool reports "Connecting… Write timeout". Tap the PWR key to wake it, then flash.
- Build output goes to `build-lcd28/` (git-ignored).

---

## Keeping up with mainline

When upstream (`mediacutlet/pocket-tank`) ships a new release:

```bat
git fetch upstream
git merge upstream/main
```

- The app core merges cleanly — the port's hardware files are additive and upstream
  never touches them.
- Conflicts, when they happen, are confined to the files you edited on top of
  upstream: mainly `firmware/main/main.c` and the seam headers.
- Build, flash and verify on the board, then `git push`.
- The whole port is also captured as one re-appliable diff (`port-lcd28.diff`, kept
  next to `POCKET_TANK_PORT_PLAN.md`) — regenerate anytime with
  `git diff upstream/main..HEAD`.

---

## License & attribution

- Upstream pocket-tank: **MIT** (see `LICENSE`, © MediaCutlet 2026).
- This port's changes: MIT, same terms.
- `firmware/main/Vernon_ST7789T/`: Waveshare demo driver, copied unmodified (protocol
  reference, courtesy of Waveshare).
- The model (`model/out/model_q4.bin`) and sounds (`assets/sounds/sounds.bin`) are
  upstream's own assets.
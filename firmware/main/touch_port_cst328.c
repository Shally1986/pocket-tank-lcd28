/* touch_port_cst328.c — the LCD28's CST328 capacitive touch (CST3530 as the
 * fallback) -> tank_touch_hold / tank_touch_tap, cloned from
 * touch_port_ft3168.c with only the chip layer + coordinate map replaced:
 * the gesture timing and every page/hit rule are the FT3168 port's (tap =
 * press+release < 350 ms with < 24 px displacement, hold > 300 ms, a drag
 * down from the top edge = feed, a moving stroke wipes algae / trims a
 * canopy, fish taps hit-test 38 px against the press-time snapshot AND the
 * current position, the stats card dismisses on a tap of empty glass, the
 * battery pill opens its page, ... - see the FT3168 port's header for the
 * full list).
 * The chip sits on its OWN I2C port 1 (SCL3/SDA1, RST2, INT4 unused - this
 * port polls), separate from the RTC + IMU bus on port 0. The vendor's
 * register protocol (main/Touch_Driver/CST328.c) is ported to the new
 * i2c_master driver: reset, the count byte at 0xD005, the packed 12-bit
 * point block at 0xD000, a 0 write to acknowledge each report.
 * The digitiser is a PORTRAIT controller under the LANDSCAPE picture: a raw
 * report is turned into the display's panel px (cst_to_panel), then through
 * the display port's view (display_port_panel_to_tank) exactly as the round
 * board's CST9217 goes in touch_port_ft3168.c; the calibration after that
 * is the CAL_* pattern of the FT3168 port. The reset confirm prompt, the
 * milestones/settings/shop/updates/battery pages and the hold-to-edit rule
 * are untouched from the FT3168 port. */
#include "sdkconfig.h"
#include "touch_port.h"
#include "display_port.h"
#include "update.h"
#include "board_pins.h"
#include "tank.h"
#include "render.h"
#include "setup.h"
#include "notice.h"
#include "audio_port.h"
#include "progression.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "touch";
static bool s_down; static int64_t s_press_us; static float s_px, s_py;
static float s_lx, s_ly;                          /* LAST touched position (release classification) */
static float s_fx[N_FISH_MAX], s_fy[N_FISH_MAX];  /* fish positions at press time */
static int s_sel = -1; static int64_t s_sel_us;   /* tapped fish -> stats card */
static bool s_ms;                                 /* milestones page up (its CLOSE button ends it) */
static bool s_cf; static int64_t s_cf_us; static int s_cf_ans;   /* reset confirm prompt */
static bool s_set;                                /* settings page up (CLOSE returns to the milestones page) */
static bool s_shop;                               /* the shop page up (CLOSE returns to the milestones page) */
static bool s_back;                               /* the settings page's CLOSE just brought the milestones page back: that
                                                     release must not reach the page as a tap on ITS CLOSE (same spot) */
static int  s_shop_act;                           /* an UNLOCK / MOVE / SELL tapped: the raw tap code, for main (one-shot) */
static bool s_held_page;                          /* this press opened a piece's page by holding on it (2026-09-24) */
static int  s_set_what, s_set_val;                /* a segment tapped: SET_TAP_* + value, for main */
static bool s_pill;                               /* main.c drew the battery pill this frame: a tap on it is its page's */
static bool s_bat; static int64_t s_bat_us;       /* the battery page up, and since when (it closes itself) */
#define BATTERY_PAGE_US (30LL * 1000000)
#define CONFIRM_TIMEOUT_US (20LL * 1000000)
static bool s_inverted;                           /* screen 180-flipped: mirror into tank space */
/* Fingers land a little BELOW where the eye aims - the pad rolls onto the
 * glass under the fingertip (phones shift their hit targets down for the
 * same reason; Strato saw it on the swatch rows, 2026-09-13). Reported
 * points move UP by this many px in displayed space; director `touch bias
 * <px>` tunes it live. */
static int s_bias_y = 10;
void touch_port_set_bias(int px) { s_bias_y = px; }
int  touch_port_bias(void) { return s_bias_y; }

void touch_port_set_inverted(bool inverted) { s_inverted = inverted; }

/* ---- the CST328 (I2C 0x1A, reset on GPIO 2, its own bus on port 1) ----
 * The vendor's register protocol (main/Touch_Driver/CST328.c) ported to the
 * new i2c_master driver: a 16-bit register (bank 0xD0 carries the report
 * block), the byte at 0xD005 is the touch count (low nibble; the high nibble
 * is a gesture/status word this port does not use), the packed 12-bit points
 * start at 0xD000, and a 0 written back to 0xD005 acknowledges each report -
 * the chip keeps the block until it is acked, so the port acks ALWAYS, even
 * a no-touch report (the vendor does the same). First point of the block:
 * x = (b1 << 4) | (b3 >> 4), y = (b2 << 4) | (b3 & 0x0F), 12 bits each. */
static i2c_master_bus_handle_t s_tbus;        /* I2C port 1, owned by this port */
static i2c_master_dev_handle_t s_cst328;
static i2c_master_dev_handle_t s_cst3530;
static uint16_t s_res_x = 240, s_res_y = 320; /* the digitiser's raw reach (the chip told us; portrait) */
static esp_err_t cst328_rd(uint16_t reg, uint8_t *d, size_t n) {
    uint8_t b[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_cst328, b, sizeof b, d, n, 20);
}
static esp_err_t cst328_wr(uint16_t reg, uint8_t v) {
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, v };
    return i2c_master_transmit(s_cst328, b, sizeof b, 20);
}
static void tp_reset(void) {                  /* the vendor's pulse: HIGH 10 ms from a LOW idle */
    gpio_config_t rst = { .pin_bit_mask = 1ULL << L28_PIN_TP_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst); gpio_sleep_sel_dis(L28_PIN_TP_RST);
    gpio_set_level(L28_PIN_TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(L28_PIN_TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
}
static i2c_master_dev_handle_t touch_dev(uint8_t addr) {
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 400000,
                                .flags.disable_ack_check = 1 };   /* it NACKs a read now and then while it scans: not an error, and not a log line each */
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(s_tbus, &cfg, &dev) != ESP_OK) dev = NULL;
    return dev;
}
static bool cst328_init(void) {
    if (i2c_master_probe(s_tbus, L28_I2C_ADDR_CST328, 50) != ESP_OK) { ESP_LOGW(TAG, "no CST328 at 0x%02x", L28_I2C_ADDR_CST328); return false; }
    s_cst328 = touch_dev(L28_I2C_ADDR_CST328);
    if (!s_cst328) return false;
    /* its debug info (the vendor's read_cfg): enter the mode, read X_MAX /
     * Y_MAX (16-bit little-endian at 0xD1F8 / 0xD1FA), back to normal mode.
     * The ranges tell us which raw axis is the glass's long side. */
    uint8_t mode[2] = { 0xD1, 0x01 }, normal[2] = { 0xD1, 0x09 }, b[2] = { 0 };
    i2c_master_transmit(s_cst328, mode, sizeof mode, 20);
    if (cst328_rd(0xD1F8, b, 2) == ESP_OK) s_res_x = (uint16_t)((b[1] << 8) | b[0]);
    if (cst328_rd(0xD1FA, b, 2) == ESP_OK) s_res_y = (uint16_t)((b[1] << 8) | b[0]);
    i2c_master_transmit(s_cst328, normal, sizeof normal, 20);
    ESP_LOGI(TAG, "CST328 ready at 0x%02x: digitiser X_MAX %d, Y_MAX %d", L28_I2C_ADDR_CST328, s_res_x, s_res_y);
    return true;
}
/* ---- the CST3530 fallback (I2C 0x58) ----
 * Hynitron's later register family: a FOUR-byte register. The report block
 * is read at 0xD0070000 (nine bytes: the touch count is byte 3's low nibble,
 * the first point's 12-bit x = ((b7 & 0x0F) << 8) | b4 and y = ((b7 & 0xF0)
 * << 4) | b5); further points would follow at 0xD0070900 (this port only
 * reads the first). The block is ended by a write to 0xD00002AB - no ack
 * byte like the CST328's, but it IS the report's ack: without it the chip
 * keeps serving the same "finger down" block for seconds (bench, stroke log
 * 2026-10-09: every tap read as a 1.9-10 s hold - no taps, no double-taps,
 * no feeds/swipes). Raw coordinates land in the same portrait 240x320
 * space. Its reset is the vendor's: LOW 100 ms then HIGH 500 ms, and it
 * answers only after that. */
static esp_err_t cst3530_rd(uint32_t reg, uint8_t *d, size_t n) {
    uint8_t b[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_cst3530, b, sizeof b, d, n, 20);
}
static void cst3530_end(void) {
    /* the END-of-report register, big-endian like every other 4-byte reg here
     * (0xD00002AB). Was 00 D0 02 AB (0x00D002AB): the first two bytes swapped,
     * so the chip never saw its ack and kept serving the same "finger down"
     * report for seconds - every tap read as a 2-10 s hold, and double-taps /
     * swipes broke with it (bench, stroke log 2026-10-09). */
    uint8_t b[4] = { 0xD0, 0x00, 0x02, 0xAB };
    i2c_master_transmit(s_cst3530, b, sizeof b, 20);
}
static bool cst3530_init(void) {
    gpio_set_level(L28_PIN_TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(L28_PIN_TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(500));
    if (i2c_master_probe(s_tbus, L28_I2C_ADDR_CST3530, 50) != ESP_OK) { ESP_LOGW(TAG, "no CST3530 at 0x%02x", L28_I2C_ADDR_CST3530); return false; }
    s_cst3530 = touch_dev(L28_I2C_ADDR_CST3530);
    if (!s_cst3530) return false;
    ESP_LOGI(TAG, "CST3530 ready at 0x%02x", L28_I2C_ADDR_CST3530);
    return true;
}
/* the chip's own sleep (M6, like the CST9217's): the CST328's deep-sleep
 * command is 0xD105 (CST328_REG_DEEP_SLEEP_MODE) and the exact handshake
 * wants a bench evening with the scope. Until then the port says false = it
 * did not take its sleep (the LCD28's deep sleep does not call this before
 * M6 wires the PWR key anyway). */
bool touch_port_deep_sleep(void) { return false; }
/* A press is one press: the chip answers some reads mid-touch with nothing (no
 * new report ready, or a NACK while it scans), and taken at their word each
 * was a lift - one finger down read as two taps (Strato, 2026-10-01). So the
 * finger is DOWN until no report has shown it for CST_LIFT_US; between
 * reports the last position stands.
 * 2026-10-03, Strato: "double taps don't always turn the light off" on the
 * round board. That silence also swallowed the gap between two quick taps
 * (one long press), and at 17 fps it was 65-95 ms before a lift showed. So a
 * real report with no finger in it is a lift at once - only
 * silence (no report ready, a NACK) is bridged. Director `touch lift <ms>` /
 * `touch said on|off` tune both live; the stroke log tells them apart. */
#define CST_LIFT_US 60000
static int  s_lift_us = CST_LIFT_US;              /* silence this long inside a press = a lift */
static bool s_lift_said = true;                   /* a report that says "no finger" lifts at once */
static int s_cst_gaps;                            /* reads inside a press that showed no finger (the stroke log) */
static int s_cst_said, s_cst_back, s_cst_maxgap_ms, s_cst_again_ms = -1;   /* ... "no finger" reports, a finger after one, the longest silence, ms since the last lift at the press */
static bool s_cst_by_said;                        /* ... and how the press ended */
static int64_t s_seen_us;                         /* the last read that showed a finger, any chip: a press is timed to it, not to the lift's bridge */
void touch_port_set_lift(int ms, int said) { if (ms > 0) s_lift_us = ms * 1000; if (said >= 0) s_lift_said = said; }
int  touch_port_lift_ms(void) { return s_lift_us / 1000; }
bool touch_port_lift_said(void) { return s_lift_said; }
static bool cst328_read(uint16_t *x, uint16_t *y) {
    static uint16_t lx, ly; static int64_t seen_us, lift_us; static bool down, said_in;
    uint8_t cnt;
    int64_t now = esp_timer_get_time();
    bool finger = false, said = false;
    if (cst328_rd(0xD005, &cnt, 1) == ESP_OK) {
        int n = cnt & 0x0F;
        if (n == 0) { said = true; cst328_wr(0xD005, 0); }          /* a report, and no finger in it */
        else if (n <= 5) {
            uint8_t d[27];
            if (cst328_rd(0xD000, d, sizeof d) == ESP_OK) {
                cst328_wr(0xD005, 0);                               /* ack: the block is consumed */
                int rx = ((d[1] << 4) | (d[3] >> 4)), ry = ((d[2] << 4) | (d[3] & 0x0F));
                if (rx > s_res_x - 1) rx = s_res_x - 1;
                if (ry > s_res_y - 1) ry = s_res_y - 1;
                lx = (uint16_t)rx; ly = (uint16_t)ry; finger = true;
            }
        } else cst328_wr(0xD005, 0);                                /* > 5 points: a bad report, ack and move on */
    }
    /* the CST9217's press rule: a report that says "no finger" lifts at
     * once (said), a missed read (no report ready, a NACK) is bridged */
    if (finger) {
        if (!down) { s_cst_gaps = s_cst_said = s_cst_back = s_cst_maxgap_ms = 0; said_in = false;
                     s_cst_again_ms = lift_us ? (int)((now - lift_us) / 1000) : -1; }
        else {
            if ((int)((now - seen_us) / 1000) > s_cst_maxgap_ms) s_cst_maxgap_ms = (int)((now - seen_us) / 1000);
            if (said_in) { s_cst_back++; said_in = false; }
        }
        seen_us = s_seen_us = now; down = true;
    } else if (down) {
        if (said) { s_cst_said++; said_in = true; }
        if ((said && s_lift_said) || now - seen_us > s_lift_us) { down = false; lift_us = now; s_cst_by_said = said && s_lift_said; }
        else s_cst_gaps++;
    }
    *x = lx; *y = ly;
    return down;
}
/* ---- the CST3530 fallback, read directly (the CST328's press rule) ---- */
static bool cst3530_read(uint16_t *x, uint16_t *y) {
    static uint16_t lx, ly; static int64_t seen_us, lift_us; static bool down, said_in;
    uint8_t d[9] = {0};
    int64_t now = esp_timer_get_time();
    bool finger = false, said = false;
    esp_err_t err = cst3530_rd(0xD0070000, d, sizeof d);
    if (err == ESP_OK) {
        int n = d[3] & 0x0F;
        if (n == 0 || (d[8] & 0xF0) == 0) said = true;              /* no finger in the report */
        else {
            int rx = ((d[7] & 0x0F) << 8) | d[4], ry = ((d[7] & 0xF0) << 4) | d[5];
            if (rx > s_res_x - 1) rx = s_res_x - 1;
            if (ry > s_res_y - 1) ry = s_res_y - 1;
            lx = (uint16_t)rx; ly = (uint16_t)ry; finger = true;
        }
        cst3530_end();                                              /* the block is consumed by the END write */
    }
    if (finger) {
        if (!down) {
            s_cst_gaps = s_cst_said = s_cst_back = s_cst_maxgap_ms = 0; said_in = false;
            s_cst_again_ms = lift_us ? (int)((now - lift_us) / 1000) : -1; }
        else {
            if ((int)((now - seen_us) / 1000) > s_cst_maxgap_ms) s_cst_maxgap_ms = (int)((now - seen_us) / 1000);
            if (said_in) { s_cst_back++; said_in = false; }
        }
        seen_us = s_seen_us = now; down = true;
    } else if (down) {
        if (said) { s_cst_said++; said_in = true; }
        if ((said && s_lift_said) || now - seen_us > s_lift_us) { down = false; lift_us = now; s_cst_by_said = said && s_lift_said; }
        else s_cst_gaps++;
    }
    *x = lx; *y = ly;
    return down;
}
/* one read of the glass, whichever chip: true while a finger is down, in panel px */
static bool panel_read(uint16_t *x, uint16_t *y) {
    if (s_cst328) return cst328_read(x, y);
    if (s_cst3530) return cst3530_read(x, y);
    return false;
}

bool touch_port_init(void) {
    i2c_master_bus_config_t bus = { .i2c_port = I2C_NUM_1, .sda_io_num = L28_PIN_TP_SDA, .scl_io_num = L28_PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    if (i2c_new_master_bus(&bus, &s_tbus) != ESP_OK) { ESP_LOGW(TAG, "no touch bus on port 1 (SCL%d, SDA%d)", L28_PIN_TP_SCL, L28_PIN_TP_SDA); return false; }
    tp_reset();
    if (cst328_init()) return true;
    if (cst3530_init()) return true;
    ESP_LOGW(TAG, "no CST328 / CST3530 on the touch bus");   /* the tank still runs, untouchable (dir's `touch` says none) */
    return false;
}

/* panel -> tank space, CALIBRATED (2026-09-30). Nine crosshairs on Strato's
 * V2 board (CST816), a press each, the panel's report against the target
 * (update mode's `ota calib`; the log fits the map):
 *
 *   target x   48  224  400      reported  22  218  424   (means over 3 rows)
 *   target y   40  184  328      reported  23  192  351   (means over 3 cols)
 *
 * so reported = 1.142 x - 33 and 1.138 y - 22 (with the old 10 px y bias
 * already inside the reported y): the panel stretches ~14% about the top
 * left. A tap at the bottom right read 25 px right and 25 px low - "aim
 * slightly left", "fingers land low near the bezel". Inverted: the same map
 * on the mirrored raw values (the panel's geometry does not turn with the
 * picture). The V1 board (FT3168) was never measured: the old bias only.
 *
 * The round board (CST9217), 2026-10-01: 63 presses on its 3 x 3
 * (tools/touch_calib.py, docs/touch/), the bias inside as above:
 *
 *   target y  110  233  356      reported  114  243  374   (sd 4..10 px a row)
 *   target x  110  233  356      reported  107  231  357   (sd 8..13 px a column)
 *
 * reported y = 1.059 y - 3.0: a press at the foot of the bowl read 18 px
 * low. In x the fit (1.015 x - 4.8, the gain +/- 0.013) cannot be told from
 * the taps' own scatter - a fingertip lands within a millimeter, 10 px on
 * this glass - so x is left as the panel says.
 *
 * The watch (FT3168), 2026-10-01: 45 presses on its 3 x 3, WORN on the
 * wrist and tapped with the other hand's finger (two sittings), the bias
 * inside:
 *
 *   target x   70  205  340      reported   82  213  342
 *   target y   70  251  432      reported   79  257  425
 *
 * reported = 0.963 x + 14.8 and 0.957 y + 13.5: this panel reads SMALL (4%),
 * where the other two read large, and worn the finger lands right of and
 * below the cross at the top left of the glass. A first sitting of 27, the
 * watch held in the hand and tapped with its thumb, sat 10 px left of these
 * (0.974 x + 5.7, 0.975 y + 14.7): the grip moves where a finger lands by
 * most of a millimeter, and a watch is used worn. */
/* the LCD28's own sitting (2026-10-09, two passes of the nine crosses via
 * calib28.py, M2 bring-up). The fit on the FULL pipeline (raw -> cst_to_panel
 * -> the display port's FIT -> cal): x press = 0.9867 t + 13.5 - the axis is
 * correct, it reads 1.3% short and 13 px right of the cross - and y press =
 * -1.0249 t + 360.7 BEFORE the mirror below was fixed (the short axis ran
 * bottom-up on the picture). The mirror is in cst_to_panel, and the gains
 * and offsets here are derived for the post-mirror pipeline: x gain 0.9867
 * off -13.5, y gain 1.0249 off +14.3 (raw fit rms 9..12 px, worst 31). The
 * y +14.3 already includes the 10 px finger bias (s_bias_y), which stays. */
#define CAL_X_GAIN 0.9867f
#define CAL_X_OFF  -13.5f
#define CAL_Y_GAIN 1.0249f
#define CAL_Y_OFF  14.3f
static void cal_point(float rx, float ry, float *tx, float *ty);
/* an upright raw report -> the finger's point on the picture as shown: calibrated, then turned with the picture */
static void cal_view(float rx, float ry, float *tx, float *ty) {
    cal_point(rx, ry, tx, ty);
    if (s_inverted) { *tx = TANK_W - 1 - *tx; *ty = TANK_H - 1 - *ty; }
}
static void cal_point(float rx, float ry, float *tx, float *ty) {                 /* a raw tank-space report -> where the finger is */
    ry -= (float)s_bias_y;                                  /* the finger's own low landing is the viewer's "down" */
    rx = (rx + CAL_X_OFF) / CAL_X_GAIN;
    ry = (ry + CAL_Y_OFF) / CAL_Y_GAIN;
    if (rx < 0) rx = 0;
    if (rx > TANK_W - 1) rx = TANK_W - 1;
    if (ry < 0) ry = 0;
    if (ry > TANK_H - 1) ry = TANK_H - 1;
    *tx = rx; *ty = ry;
}
/* the panel's raw reach, as seen since boot (director `touch`): the stroke's
 * reach below takes it for 0..max on both axes - scrub the four walls, then ask */
static int s_raw_x0 = TANK_W, s_raw_x1 = -1, s_raw_y0 = TANK_H, s_raw_y1 = -1;
void touch_port_raw_seen(int *x0, int *x1, int *y0, int *y1) { *x0 = s_raw_x0; *x1 = s_raw_x1; *y0 = s_raw_y0; *y1 = s_raw_y1; }
/* the digitiser's raw report -> the display's panel px (the landscape 320x240
 * space display_port_panel_to_tank expects). The digitiser scans the glass in
 * the module's native PORTRAIT 240x320, so a raw (rx, ry) is turned like the
 * picture is. 2026-10-09 (M2 bring-up, CALIBRATED): the raw LONG axis
 * (s_res_y, Y_MAX) runs left-right on the landscape view, confirmed on the
 * bench (top-row crosses read at gain +0.987 on x). The raw SHORT axis
 * (s_res_x) ran BOTTOM-up - pressing the top crosses read at the bottom of
 * the display (fit gain -1.025) - so the short axis is MIRRORED here: the
 * chip's rx = 0 is the display's bottom edge, rx max is its top. The mirror
 * below plus the CAL_* gains/offsets above is the whole transformation. */
static void cst_to_panel(uint16_t rx, uint16_t ry, int *dx, int *dy) {
    *dx = (int)ry;                              /* long axis -> the landscape width */
    *dy = (int)(s_res_x - 1) - (int)rx;         /* short axis -> the landscape height, mirrored */
}
static void map_touch(uint16_t px, uint16_t py, float *tx, float *ty) {
    int dx, dy;
    cst_to_panel(px, py, &dx, &dy);
    float fx, fy;
    display_port_panel_to_tank(dx, dy, &fx, &fy);   /* the display port's view: the FIT into the 448x368 frame */
    int rx = (int)fx, ry = (int)fy;
    /* the raw report in tank space AS IF UPRIGHT: the calibration is done in
     * that frame and the 180-degree turn comes after it (cal_view), the FT3168
     * port's rule - the display's FIT flip never touches these coords. */
    if (rx < s_raw_x0) s_raw_x0 = rx;
    if (rx > s_raw_x1) s_raw_x1 = rx;
    if (ry < s_raw_y0) s_raw_y0 = ry;
    if (ry > s_raw_y1) s_raw_y1 = ry;
    cal_view((float)rx, (float)ry, tx, ty);
}
/* The STROKE's reach to the glass (2026-10-01). The panel's report runs out
 * before the glass does: its raw 0..max lands, through the map above, on x
 * 29..420 and y 11..333 - a finger on the outer 29 px (the bottom 35) reads
 * as the limit, never beyond. Taps want exactly that map (the crosshairs),
 * but a scrub could no longer reach the film in the edge cells and a slash
 * stopped short of a bed's outer frond and of the floor (Strato: "difficult
 * if not impossible to scrub algae which sits too close to the glass edge",
 * "the farthest left and most far right seagrass fronds cannot be clipped").
 * Before the calibration the stretched report overshot to the edge on its
 * own. So the stream to tank_touch_drag - and nothing else - is stretched
 * over the last EDGE_BAND_* px inside each limit so the limit lands on the
 * glass (EDGE_SNAP_* short of it already does: a pad at the bezel does not
 * drive the report to its very end). A stroke keeps the finger's own
 * travel for its first EDGE_RAMP0_PX and eases into the full stretch by
 * EDGE_RAMP1_PX: a tap's roll is not magnified into a wipe or a snip (tank.c
 * WIPE_ENGAGE_PX / SLASH_PX), a real stroke runs to the glass. It LANDS
 * fully stretched in y (a sweep along the floor is on the floor from its
 * first frame, and stays level) but only EDGE_LAND_X of the way in x: a
 * finger put down on a bed's outer frond, 24 px from the wall, reads as the
 * panel's limit - landed at the glass it would be a whole pad's width off
 * the spine and arm no scissors (tank.c SLASH_START_SIDE_PX); landed half
 * way it is beside the spine with the frond inside its reach.
 * One exception to the ramp, x only: travel TOWARD a side wall is stretched
 * from its first px. A swipe at a wall frond is a short thing in the report -
 * the pad runs into the bezel and its center stops 15..25 px short of the
 * limit - so it never travelled far enough to open the ramp, or to be a
 * slash at all (Strato's log, 2026-10-01: starts at x 43..71, lifts at
 * 39..53, 4..20 px apart). The price: within the band, a tap that rolls 8 px
 * toward the wall on a tall frond can snip it. */
#define EDGE_BAND_X   44.0f
#define EDGE_SNAP_X    8.0f
#define EDGE_BAND_Y   44.0f
#define EDGE_SNAP_Y    8.0f
#define EDGE_RAMP0_PX 18.0f
#define EDGE_RAMP1_PX 42.0f
#define EDGE_LAND_X    0.5f
static float s_sx, s_sy, s_reach;                 /* this stroke: its landing point, how far the stretch is in (0..1) */
static float s_ex0;                               /* ... and its landing x fully stretched */
static bool  s_log;                               /* director `touch log on`: one line per press, at its release */
static float s_dx0, s_dx1, s_dlx, s_dly; static int s_frames, s_raw_px, s_raw_py;   /* ... what it reports */
void touch_port_set_log(bool on) { s_log = on; }
static float edge_stretch1(float v, float lo, float hi, float max, float band, float snap) {
    if (lo > 0 && v < lo + band) {
        float e = v - lo - snap;
        return e <= 0 ? 0 : e * (lo + band) / (band - snap);
    }
    if (hi < max && v > hi - band) {
        float e = hi - v - snap;
        return e <= 0 ? max : max - e * (max - hi + band) / (band - snap);
    }
    return v;
}
static void edge_stretch(float x, float y, float *ox, float *oy) {
    float x0, y0, x1, y1;                         /* the panel's reach: its raw extremes through the same map (the bias is live) */
    cal_view(0, 0, &x0, &y0); cal_view(TANK_W - 1, TANK_H - 1, &x1, &y1);
    if (x0 > x1) { float k = x0; x0 = x1; x1 = k; }   /* turned: the far corner is the near one */
    if (y0 > y1) { float k = y0; y0 = y1; y1 = k; }
    *ox = edge_stretch1(x, x0, x1, TANK_W - 1, EDGE_BAND_X, EDGE_SNAP_X); *oy = edge_stretch1(y, y0, y1, TANK_H - 1, EDGE_BAND_Y, EDGE_SNAP_Y);
}
/* the calibrated point of a touched frame -> the stroke's point for tank_touch_drag */
static void stroke_point(bool landed, float tx, float ty, float *dx, float *dy) {
    float ex, ey; edge_stretch(tx, ty, &ex, &ey);
    if (landed) { s_sx = tx + (ex - tx) * EDGE_LAND_X; s_sy = ey; s_ex0 = ex; s_reach = 0; }
    float d = hypotf(tx - s_px, ty - s_py);
    float r = (d - EDGE_RAMP0_PX) / (EDGE_RAMP1_PX - EDGE_RAMP0_PX);
    if (r > 1) r = 1;
    if (r > s_reach) s_reach = r;                 /* latched: scrubbing back past the landing point stays stretched */
    float ux = s_sx + tx - s_px;                  /* the finger's own travel from its landing ... */
    if ((ex - tx) * (tx - s_px) > 0) ux = s_sx + ex - s_ex0;   /* ... but stretched at once on its way TO a wall (below) */
    *dx = ux * (1 - s_reach) + ex * s_reach;
    *dy = (s_sy + ty - s_py) * (1 - s_reach) + ey * s_reach;
    if (*dx < 0) *dx = 0;
    if (*dx > TANK_W - 1) *dx = TANK_W - 1;
    if (*dy < 0) *dy = 0;
    if (*dy > TANK_H - 1) *dy = TANK_H - 1;
}
static bool s_upd; static int s_upd_act;                 /* the UPDATES page (2026-09-30) */
bool touch_port_read_raw(float *x, float *y) {
    uint16_t px[1], py[1];
    if (!panel_read(px, py)) return false;
    map_touch(px[0], py[0], x, y);
    return true;
}

/* call every frame from the tank task */
void touch_port_poll(tank_t *t) {
    int64_t now = esp_timer_get_time();
    if (s_cf && now - s_cf_us > CONFIRM_TIMEOUT_US) touch_port_confirm_answer(-1);   /* nobody answered: keep the tank */
    if (!s_cst328 && !s_cst3530) return;   /* no chip answered: the tank runs, untouchable */
    uint16_t x[1], y[1];
    bool touched = panel_read(x, y);
    /* the digitiser's raw report -> tank space is map_touch's (above): calibrated, then turned */
    float tx = s_lx, ty = s_ly;
    if (touched) map_touch(x[0], y[0], &tx, &ty);           /* calibrated (above); the release keeps the last position */
    if (touched && !s_down) {
        audio_port_prewarm();                   /* the release's cue plays warm */
        s_press_us = now; s_px = tx; s_py = ty;
        /* snapshot the school: the user aims at where a fish WAS - by release
           a darting fish has moved and the finger hid it the whole time */
        for (int i = 0; i < t->n_fish && i < N_FISH_MAX; i++) { s_fx[i] = t->fish[i].x; s_fy[i] = t->fish[i].y; }
    }
    bool su = setup_active();                                /* before the touch: BEGIN's release is not a tank tap */
    if (s_set && !s_cf && !su) {                             /* the settings page owns the glass: segments, the seconds wheel, CLOSE */
        int v = 0, r = render_settings_touch(t, tx, ty, touched, &v);
        if (r) ESP_LOGI(TAG, "settings: %s %d", r == SET_TAP_CLOSE ? "CLOSE" : r == SET_TAP_BRIGHT ? "brightness" : r == SET_TAP_VOLUME ? "volume"
                                                  : r == SET_TAP_LIGHT ? "lights out" : r == SET_TAP_SCREEN ? "screen (1 = turned)"
                                                  : r == SET_TAP_FEED ? "auto feed (1 = on)" : r == SET_TAP_ROTATE ? "rotation (1 = locked)" : "idle seconds", v);
        if (r == SET_TAP_CLOSE) { s_set = false; s_ms = true; s_back = true; }   /* back to the milestones page (2026-09-16); the release is spent */
        else if (r == SET_TAP_UPDATES) { s_set = false; s_upd = true; s_back = true; ESP_LOGI(TAG, "updates page up"); }
        else if (r == SET_TAP_BRIGHT || r == SET_TAP_VOLUME || r == SET_TAP_LIGHT || r == SET_TAP_IDLE || r == SET_TAP_FEED || r == SET_TAP_ROTATE) { s_set_what = r; s_set_val = v; }
    }
    if (s_upd && !s_cf && !su) {                             /* the UPDATES page: CHECK (main restarts), FORGET, CLOSE */
        int r = updates_page_touch(tx, ty, touched);
        if (r == UPD_TAP_CLOSE) { s_upd = false; s_set = true; s_back = true; ESP_LOGI(TAG, "updates page: CLOSE -> settings"); }
        else if (r == UPD_TAP_CHECK) { s_upd_act = r; ESP_LOGI(TAG, "updates page: CHECK FOR UPDATES"); }
        else if (r == UPD_TAP_FORGET) ESP_LOGI(TAG, "updates page: network forgotten");
    }
    if (su && !s_cf) {
        bool birth = setup_is_birth(), rename = setup_is_rename(); int who = setup_fish(), place = setup_item();
        setup_touch(t, tx, ty, touched);                     /* taps and the letter wheel, classified in setup.c */
        if (!setup_active()) {
            if (rename) ESP_LOGI(TAG, "rename closed: the fish is %s", who >= 0 && who < t->n_fish ? t->fish[who].name : "?");
            else if (birth) ESP_LOGI(TAG, "birth flow done: %s named and saved", who >= 0 && who < t->n_fish ? t->fish[who].name : "?");
            else if (place >= 0) ESP_LOGI(TAG, "placed: %s at x %.0f, %s layer, saved", SD_ITEMS[place].name, tank_decor_x(t, place),
                                          tank_decor_z(t, place) == DECOR_Z_BACK ? "BEHIND" : tank_decor_z(t, place) == DECOR_Z_FRONT ? "IN FRONT" : "AMONG");
            else ESP_LOGI(TAG, "setup done: %s + %s", t->fish[0].name, t->fish[1].name);
        }
    }
    { int rf = setup_take_renamed();                         /* a rename closed (2026-10-01): back to the milestones page, the fish's card up */
      if (rf >= 0) { s_ms = true; s_set = false; s_shop = false; s_sel = -1; render_milestones_show_fish(t, rf); } }
    bool modal = s_ms || s_set || s_shop || s_cf || su || s_bat || s_upd;   /* a page or a prompt owns the glass */
    if (touched) {                                           /* stroke = wipe/slash, reaching the glass (stroke_point above) */
        float dx, dy; stroke_point(!s_down, tx, ty, &dx, &dy);
        if (!s_down) { s_dx0 = s_dx1 = dx; s_frames = 0; s_raw_px = x[0]; s_raw_py = y[0]; }
        if (dx < s_dx0) s_dx0 = dx;
        if (dx > s_dx1) s_dx1 = dx;
        s_dlx = dx; s_dly = dy; s_frames++;
        s_lx = tx; s_ly = ty; if (!modal) tank_touch_drag(t, dx, dy);
    }
    /* tap-and-hold on a decoration (2026-09-24, Strato: "right now it's like 5
       taps to get back to the edit"): a still finger 700 ms on an owned
       piece opens its placement page (MOVE / DEPTH / SELL) right there */
    if (touched && !modal && !s_held_page && t->tool == TOOL_HAND && now - s_press_us > 700000 && fabsf(tx - s_px) < 24 && fabsf(ty - s_py) < 24) {
        int it = tank_decor_hit(t, s_px, s_py);
        if (it >= 0) { setup_begin_place(t, it); s_held_page = true; s_sel = -1;
                       ESP_LOGI(TAG, "held on the %s: placement page up", SD_ITEMS[it].name); }
    }
    if (touched && !modal && !s_held_page && now - s_press_us > 300000 && fabsf(ty - s_py) < 30) tank_touch_hold(t, tx, ty);
    if (!touched && s_down) {
        /* release: classify with the LAST touched position (the old code fell
           back to the PRESS position here, so dx/dy were always 0 - every
           quick swipe read as a tap and the drag-feed could never fire) */
        float dx = s_lx - s_px, dy = s_ly - s_py;
        int64_t held_us = s_seen_us - s_press_us;      /* the finger's own time on the glass: the lift's bridge is not the keeper's */
        if (s_log) ESP_LOGI(TAG, "stroke: raw %d,%d | press %.0f,%.0f -> lift %.0f,%.0f | lands %.0f,%.0f, drag x %.0f..%.0f, ends %.0f,%.0f | %d frames %d ms%s | scissors %s%s%s, travel %.0f",
                            s_raw_px, s_raw_py, s_px, s_py, s_lx, s_ly, s_sx, s_sy, s_dx0, s_dx1, s_dlx, s_dly, s_frames, (int)(held_us / 1000),
                            modal ? " (a page had the glass)" : "", t->slash_armed ? "ARMED" : "not armed", t->slash_engaged ? " ENGAGED" : "", t->slash_cut ? " CUT" : "", t->drag_dist);
        if (s_log && (s_cst328 || s_cst3530)) ESP_LOGI(TAG, "stroke: lift by %s (%d ms rule, said %s) | %d silent reads bridged, longest silence %d ms | %d \"no finger\" reports, a finger again after %d | pressed %d ms after the last lift | lift seen %d ms after the finger",
                                     s_cst_by_said ? "the chip's word" : "silence", s_lift_us / 1000, s_lift_said ? "on" : "off", s_cst_gaps, s_cst_maxgap_ms, s_cst_said, s_cst_back, s_cst_again_ms, (int)((now - s_seen_us) / 1000));
        if (s_cf) {                     /* the prompt owns the glass: a press AND release on the
                                           same button answers it, nothing else counts - not
                                           even the tap that opened it (it began before) */
            int h = s_press_us > s_cf_us ? render_confirm_hit(s_px, s_py) : 0;
            if (h && h == render_confirm_hit(s_lx, s_ly)) touch_port_confirm_answer(h);
            goto released;
        }
        if (su) {                       /* the setup had the glass (setup_touch above); just the log:
                                           where the finger landed vs what it hit, in case this panel
                                           reports fingers offset from where they feel */
            ESP_LOGI(TAG, "setup touch press %.0f,%.0f release %.0f,%.0f -> %s", s_px, s_py, s_lx, s_ly,
                     setup_hit_name(setup_active() ? setup_hit(s_px, s_py) : 0));
            s_sel = -1; goto released;
        }
        if (s_bat) { s_bat = false; ESP_LOGI(TAG, "battery page closed"); goto released; }   /* any release closes it */
        if (held_us < 350000 && dx * dx + dy * dy < 24 * 24) {
            if (notice_current()) {                                 /* (the lights-out notice lets its tap through: notice.h) */
                bool took = notice_dismiss(); ESP_LOGI(TAG, "tap closed the announcement"); if (took) goto released; }
            if (s_set || s_upd || s_back) { s_back = false; goto released; }   /* the settings / updates page had the glass (their touch calls above) */
            if (s_shop) {                                           /* the shop: a row's modal, UNLOCK, HOW TO EARN, CLOSE */
                int r = render_shop_tap(t, s_px, s_py);
                ESP_LOGI(TAG, "shop tap at %.0f,%.0f -> %s", s_px, s_py, r == SHOP_TAP_CLOSE ? "CLOSE" : r >= SHOP_TAP_SELL ? "SELL" : r >= SHOP_TAP_MOVE ? "MOVE" : r >= SHOP_TAP_BUY ? "UNLOCK" : r == SHOP_TAP_KEPT ? "modal" : "nothing");
                if (r == SHOP_TAP_CLOSE) { s_shop = false; render_shop_leave(); s_ms = true; }   /* back to the milestones page (2026-09-16) */
                else if (r >= SHOP_TAP_BUY) s_shop_act = r;   /* main.c buys (and plays the cue) or opens the placement page */
                goto released;
            }
            if (s_ms) {                                             /* the page: badges open a modal, the CLOSE
                                                                       button ends it, the brightness row cycles */
                int r = render_milestones_tap(t, s_px, s_py);     /* CLOSE / SETTINGS / the sand dollar, detail modal, nothing */
                ESP_LOGI(TAG, "page tap at %.0f,%.0f (release %.0f,%.0f) -> %s", s_px, s_py, s_lx, s_ly,
                         r >= MS_TAP_SELL ? "SELL (a fish's card)" : r >= MS_TAP_RENAME ? "RENAME (a fish's card)" :
                         r == MS_TAP_CLOSE ? "CLOSE" : r == MS_TAP_SETTINGS ? "SETTINGS" : r == MS_TAP_SHOP ? "SHOP" : r == MS_TAP_KEPT ? "detail" : "nothing");
                if (r >= MS_TAP_SELL) {                             /* a fish's card: SELL, confirmed - the page stays, a row shorter */
                    int fi = r - MS_TAP_SELL, worth = progression_fish_value(t, fi);
                    char nm[FISH_NAME_MAX + 1]; snprintf(nm, sizeof nm, "%s", fi < t->n_fish ? t->fish[fi].name : "?");
                    if (progression_sell_fish(t, fi)) { s_sel = -1; ESP_LOGI(TAG, "%s sold for %d: %d fish left, balance %d", nm, worth, t->n_fish, (int)t->sd_balance); }
                    goto released;
                }
                if (r >= MS_TAP_RENAME) {                           /* ... RENAME: the letter wheel over the live tank */
                    s_ms = false; s_sel = -1; progression_ack_milestones(t); render_milestones_leave();
                    setup_begin_rename(t, r - MS_TAP_RENAME);
                    goto released;
                }
                if (r != MS_TAP_CLOSE && r != MS_TAP_SETTINGS && r != MS_TAP_SHOP) goto released;   /* only a button leaves the page */
                s_ms = false; s_sel = -1; s_set = r == MS_TAP_SETTINGS; s_shop = r == MS_TAP_SHOP;
                progression_ack_milestones(t); render_milestones_leave();   /* everything shown is now "seen" */
                goto released;
            }
            if (t->tool != TOOL_HAND) {                      /* a tool in hand owns the glass (2026-10-04): only its chip's DONE answers a tap */
                s_sel = -1;
                if (render_tool_chip_hit(t, s_px, s_py)) { tank_set_tool(t, TOOL_HAND); ESP_LOGI(TAG, "tool chip tapped at %.0f,%.0f: back to bare hands", s_px, s_py); }
                else ESP_LOGI(TAG, "tap at %.0f,%.0f ignored: a tool is in hand (DONE first)", s_px, s_py);
                goto released;
            }
            if (s_pill && RENDER_BAT_HIT(s_px, s_py)) {      /* the battery pill, while it shows: its page (2026-09-24) */
                s_bat = true; s_bat_us = now; s_sel = -1;
                ESP_LOGI(TAG, "battery pill tapped at %.0f,%.0f: battery page up", s_px, s_py);
                goto released;
            }
            if (s_sel >= 0 && s_sel < t->n_fish && render_tools_hit(s_px, s_py) >= 0) {   /* the toolbox under the card (2026-10-01):
                                                                                     pick a tool up, or put the one in hand back */
                int tl = render_tools_hit(s_px, s_py);
                tank_set_tool(t, t->tool == tl ? TOOL_HAND : tl); s_sel = -1;
                ESP_LOGI(TAG, "toolbox tap at %.0f,%.0f -> %s", s_px, s_py, t->tool == TOOL_SPONGE ? "SPONGE" : t->tool == TOOL_SCISSORS ? "SCISSORS" : "bare hands");
                goto released;
            }
            if ((s_sel < 0 || s_sel >= t->n_fish) && render_tool_chip_hit(t, s_px, s_py)) {   /* the tool chip's DONE */
                tank_set_tool(t, TOOL_HAND);
                ESP_LOGI(TAG, "tool chip tapped at %.0f,%.0f: back to bare hands", s_px, s_py);
                goto released;
            }
            if (s_sel >= 0 && s_sel != RENDER_CARD_SNAIL && s_sel != RENDER_CARD_SHRIMP && s_sel != RENDER_CARD_URCHIN && RENDER_CARD_HIT(s_px, s_py)) {   /* a tap ON the card (or the slop
                ESP_LOGI(TAG, "card tap at %.0f,%.0f -> milestones", s_px, s_py);         under its MORE button) = milestones page */
                s_ms = true; goto released;
            }
            /* fish first; only an empty tap reaches the water. 38 px radius
               (a fingertip on this 322 ppi panel covers ~60 px) against BOTH
               the press-time snapshot and the current position - whichever is
               closer - so a fish that moved mid-tap still registers. */
            int best = -1; float bd = 38 * 38;
            for (int i = 0; i < t->n_fish; i++) {
                float ax = s_fx[i] - s_px, ay = s_fy[i] - s_py;
                float bx = t->fish[i].x - s_px, by = t->fish[i].y - s_py;
                float d2a = ax * ax + ay * ay, d2b = bx * bx + by * by;
                float d2 = d2a < d2b ? d2a : d2b;
                if (d2 < bd) { bd = d2; best = i; }
            }
            if (best >= 0) { s_sel = (best == s_sel) ? -1 : best; s_sel_us = now; }
            else if (tank_snail_hit(t, s_px, s_py)) {   /* the snail: its card (2026-09-16), the fish first */
                s_sel = s_sel == RENDER_CARD_SNAIL ? -1 : RENDER_CARD_SNAIL; s_sel_us = now;
                ESP_LOGI(TAG, "snail tapped: card %s (%d spots grazed)", s_sel >= 0 ? "up" : "down", (int)t->snail_grazed); }
            else if (tank_urchin_hit(t, s_px, s_py)) {  /* the urchin: its card (2026-10-02) */
                s_sel = s_sel == RENDER_CARD_URCHIN ? -1 : RENDER_CARD_URCHIN; s_sel_us = now;
                ESP_LOGI(TAG, "urchin tapped: card %s (%.0f cm of grass grazed)", s_sel >= 0 ? "up" : "down", t->urchin_grazed_px / PX_PER_CM); }
            else if (tank_shrimp_hit(t, s_px, s_py)) {  /* the shrimp school (2026-09-29): a tap its card, the third quick tap scares them */
                int taps = tank_shrimp_tap(t, s_px, s_py);
                if (taps == 1) { s_sel = s_sel == RENDER_CARD_SHRIMP ? -1 : RENDER_CARD_SHRIMP; s_sel_us = now; }
                else if (taps >= 3) s_sel = -1;
                ESP_LOGI(TAG, "shrimp tapped (%d): card %s | %d shrimp, %d eaten, %d/%d to the next", taps, s_sel == RENDER_CARD_SHRIMP ? "up" : "down",
                         t->shrimp_n, (int)t->shrimp_eaten, t->shrimp_food, SHRIMP_PER_JOIN); }
            else if (s_sel >= 0) s_sel = -1;   /* card up: a tap on empty glass just
                                                  dismisses it - it is NOT a tank tap
                                                  (no feed, no light-toggle burst) */
            else tank_touch_tap(t, s_px, s_py);
        }
        else if (s_ms && !s_back && fabsf(dx) >= 40 && fabsf(dx) > 2 * fabsf(dy)) {   /* sideways along the TANK row: its page (2026-09-30) */
            if (render_milestones_swipe(t, s_px, s_py, dx)) ESP_LOGI(TAG, "page swipe %.0f px on the tank row", dx);
        }
        else if (!s_ms && t->tool == TOOL_HAND && s_py - tank_glass_top(s_px) < 60 && dy >= 40) tank_feed(t, s_lx, 3);  /* drag down from the top = feed (not with a tool in hand) */
    }
released:
    if (!touched) s_held_page = false;
    s_down = touched;
    if (s_sel >= t->n_fish && s_sel != RENDER_CARD_SNAIL && s_sel != RENDER_CARD_SHRIMP && s_sel != RENDER_CARD_URCHIN) s_sel = -1;   /* fresh tank / save load */
    if (s_sel >= 0 && now - s_sel_us > 10 * 1000000) s_sel = -1; /* auto-dismiss */
    if (s_bat && now - s_bat_us > BATTERY_PAGE_US) s_bat = false; /* the battery page too, after a while */
}

int touch_port_selected(void) { return s_sel; }
bool touch_port_milestones(void) { return s_ms; }
void touch_port_show_milestones(bool on) { if (s_ms && !on) render_milestones_leave(); s_ms = on; }
void touch_port_dismiss(void) { s_sel = -1; if (s_ms) render_milestones_leave(); if (s_shop) render_shop_leave(); s_ms = false; s_set = false; s_shop = false; s_bat = false; s_upd = false; }
bool touch_port_updates(void) { return s_upd; }
void touch_port_show_updates(bool on) { s_upd = on; if (on) { s_ms = false; s_set = false; s_shop = false; s_sel = -1; s_bat = false; } }
int  touch_port_take_update(void) { int r = s_upd_act; s_upd_act = 0; return r; }

/* ---- reset confirm prompt ---- */
void touch_port_confirm_open(void) {
    s_cf = true; s_cf_us = esp_timer_get_time(); s_cf_ans = 0;
    s_sel = -1; s_ms = false; s_set = false; s_shop = false; s_bat = false; s_upd = false; render_shop_leave();   /* it replaces the card / the pages */
    ESP_LOGI(TAG, "reset prompt up (YES / NO on the glass; NO by itself in %d s)", (int)(CONFIRM_TIMEOUT_US / 1000000));
}
bool touch_port_confirm_answer(int ans) {
    if (!s_cf) return false;
    s_cf = false; s_cf_ans = ans > 0 ? 1 : -1;
    return true;
}
bool  touch_port_confirm_up(void)   { return s_cf; }
float touch_port_confirm_frac(void) {
    if (!s_cf) return 0;
    float f = 1.0f - (esp_timer_get_time() - s_cf_us) / (float)CONFIRM_TIMEOUT_US;
    return f < 0 ? 0 : f;
}
int  touch_port_confirm_take(void)  { int a = s_cf_ans; s_cf_ans = 0; return a; }
bool touch_port_pressed_since(int64_t us) { return s_down && s_press_us > us; }
bool touch_port_settings(void) { return s_set; }
void touch_port_show_settings(bool on) { s_set = on; if (on) { s_ms = false; s_sel = -1; s_bat = false; } }
int  touch_port_take_setting(int *value) { int w = s_set_what; *value = s_set_val; s_set_what = 0; return w; }
bool touch_port_shop(void) { return s_shop; }
void touch_port_show_shop(bool on) { if (s_shop && !on) render_shop_leave(); s_shop = on; if (on) { s_ms = false; s_set = false; s_sel = -1; s_bat = false; } }
int  touch_port_take_shop(void) { int r = s_shop_act; s_shop_act = 0; return r; }
void touch_port_set_pill(bool up) { s_pill = up; }
bool touch_port_battery(void) { return s_bat; }
void touch_port_show_battery(bool on) { s_bat = on; if (on) { s_bat_us = esp_timer_get_time(); s_sel = -1; } }

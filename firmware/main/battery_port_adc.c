/* battery_port_adc.c — the ESP32-S3-Touch-LCD-2.8-V2 battery meter + PWR key.
 *
 * This board has NO PMIC. Unlike the Waveshare AMOLED boards (AXP2101), the
 * cell voltage comes in through a 3:1 divider to GPIO8 (ADC1 channel 7) and
 * is read with the `adc_oneshot` driver + eFuse calibration, exactly as the
 * vendor BAT_Driver does (`(mV * 3.0 / 1000) / 0.9945`). There is no
 * charger-detect or charge-status line broken out, so charging is always
 * false and the state is always BAT_ON_BATTERY — the pill shows the level
 * only, never the bolt.
 *
 * Power on this board is held by a latch: GPIO7 high keeps the battery
 * connected, GPIO6 is the PWR key (active low). board_early_init() (called as
 * the very first line of app_main) latches the rail within microseconds of
 * boot; battery_port_poweroff() releases it. The key itself is a small
 * press-time state machine here (the vendor's PWR_Loop in PWR_Key.c):
 *   short press  -> 1  (main.c: the sleep flow)
 *   long  press  -> 2  (main.c: power off now)
 * The vendor's tick thresholds (sleep 10, restart 15, shutdown 20 ticks) are
 * ~1.0/1.5/2.0 s at its 100 ms tick; pocket-tank only needs the short/long
 * split, so it is expressed in ms. */
#include "battery_port.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHAN     ADC_CHANNEL_7          /* GPIO8 on this board */
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_12
#define BAT_DIVIDER      3.0f                   /* the on-board divider */
#define BAT_CAL_OFFSET   0.994500f              /* the vendor's Measurement_offset */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;
static int64_t s_last_us = -1;
static float s_frac;
static bool s_valid;

/* ---- the power-hold latch + the PWR key ---------------------------------- */

/* must be the first thing app_main does: on battery the board cuts out if the
 * hold line is not driven high promptly (see POCKET_TANK_PORT_PLAN.md 8.3). */
void board_early_init(void)
{
    gpio_config_t in = { .pin_bit_mask = 1ULL << L28_PIN_KEY, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    gpio_config_t out = { .pin_bit_mask = 1ULL << L28_PIN_HOLD, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(L28_PIN_HOLD, 1);            /* latch the battery on */
    gpio_hold_en(L28_PIN_HOLD);                 /* survive a reset / deep sleep */
    ESP_LOGI(TAG, "power hold latched (GPIO%d high), PWR key on GPIO%d",
             L28_PIN_HOLD, L28_PIN_KEY);
}

void battery_port_key_init(void)
{
    /* board_early_init already configured GPIO6 as an input with a pull-up.
     * Nothing else to arm - the key is polled, not interrupt-driven, like the
     * AMOLED boards' PMIC IRQ flags. */
    ESP_LOGI(TAG, "PWR key ready (GPIO%d, short = sleep, long = power off)", L28_PIN_KEY);
}

#define KEY_DEBOUNCE_MS 30
#define KEY_LONG_MS     1500
static bool s_key_down;
static int64_t s_key_down_us;
static bool s_key_long_sent;
static int s_key_event;

int battery_port_key_poll(void)
{
    if (s_key_event) { int e = s_key_event; s_key_event = 0; return e; }
    int level = gpio_get_level(L28_PIN_KEY);
    int64_t now = esp_timer_get_time();
    if (!level) {                               /* pressed (active low) */
        if (!s_key_down) { s_key_down = true; s_key_down_us = now; s_key_long_sent = false; }
        else if (!s_key_long_sent && now - s_key_down_us >= KEY_LONG_MS * 1000) {
            s_key_long_sent = true;             /* long press fires once, while still held */
            s_key_event = 2;
        }
    } else if (s_key_down) {                    /* released */
        s_key_down = false;
        if (!s_key_long_sent && now - s_key_down_us >= KEY_DEBOUNCE_MS * 1000)
            s_key_event = 1;
    }
    if (s_key_event) { int e = s_key_event; s_key_event = 0; return e; }
    return 0;
}

void battery_port_key_trace(int seconds)
{
    ESP_LOGI(TAG, "key trace: press the PWR key a few times in the next %d s (nothing sleeps)", seconds);
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    bool down = false; int64_t down_us = 0; int n = 0;
    while (esp_timer_get_time() < end) {
        bool now_down = gpio_get_level(L28_PIN_KEY) == 0;
        if (now_down && !down) { down = true; down_us = esp_timer_get_time(); }
        else if (!now_down && down) {
            down = false; n++;
            ESP_LOGI(TAG, "key trace: press %d held %d ms%s", n,
                     (int)((esp_timer_get_time() - down_us) / 1000),
                     (esp_timer_get_time() - down_us) < 128000 ? "  <-- under 128 ms" : "");
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "key trace over: %d presses", n);
}

bool battery_port_poweroff(void)
{
    ESP_LOGW(TAG, "power off: releasing the hold latch (GPIO%d low); the PWR key boots again", L28_PIN_HOLD);
    gpio_hold_dis(L28_PIN_HOLD);
    gpio_set_level(L28_PIN_HOLD, 0);
    return true;                                 /* the rails drop, unless USB is still feeding the board */
}

/* ---- the gauge ----------------------------------------------------------- */

/* the cell's resting curve (single-cell Li-ion); the AXP2101 gave a coulomb
 * count, this board only has the divider, so percent is inferred from voltage */
static int lipo_pct(float v)
{
    static const float mv[]  = { 3.20f, 3.40f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.20f };
    static const int   pct[] = {    0,     5,    20,    35,    50,    65,    80,   100 };
    if (v <= mv[0]) return 0;
    for (int i = 1; i < 8; i++) {
        if (v < mv[i]) {
            float f = (v - mv[i - 1]) / (mv[i] - mv[i - 1]);
            return pct[i - 1] + (int)(f * (pct[i] - pct[i - 1]) + 0.5f);
        }
    }
    return 100;
}

static bool adc_calibration_init(void)
{
    bool calibrated = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = BAT_ADC_UNIT, .chan = BAT_ADC_CHAN,
        .atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, &s_cali) == ESP_OK) calibrated = true;
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cfg = {
            .unit_id = BAT_ADC_UNIT, .atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cfg, &s_cali) == ESP_OK) calibrated = true;
    }
#endif
    if (!calibrated) ESP_LOGW(TAG, "eFuse calibration unavailable: the gauge reads uncalibrated");
    return calibrated;
}

/* one ADC sample -> the battery voltage in mV (after the divider + the
 * vendor's measured offset correction) */
static int read_mv(void)
{
    if (!s_adc) return 0;
    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw) != ESP_OK) return 0;
    int mv = 0;
    if (s_cali_ok) adc_cali_raw_to_voltage(s_cali, raw, &mv);
    else mv = raw * 3300 / 4095;                 /* rough 12-bit fallback */
    return (int)(mv * BAT_DIVIDER / BAT_CAL_OFFSET + 0.5f);
}

bool battery_port_init(i2c_master_bus_handle_t bus)
{
    (void)bus;                                  /* the meter is on the ADC, not I2C */
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 init failed: battery meter hidden");
        s_adc = NULL;
        return false;
    }
    adc_oneshot_chan_cfg_t chan = { .atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHAN, &chan) != ESP_OK) {
        ESP_LOGW(TAG, "ADC1 channel %d config failed", (int)BAT_ADC_CHAN);
        return false;
    }
    s_cali_ok = adc_calibration_init();
    ESP_LOGI(TAG, "battery ADC1_CH%d (GPIO%d) up%s | %d mV", (int)BAT_ADC_CHAN,
             L28_PIN_BAT_ADC, s_cali_ok ? "" : " (uncalibrated)", read_mv());
    return true;
}

bool battery_port_read(float *frac, bool *charging)
{
    int64_t now = esp_timer_get_time();
    if (s_last_us < 0 || now - s_last_us > 900000) {
        s_last_us = now;
        int mv = read_mv();
        if (mv > 0) { s_frac = lipo_pct(mv / 1000.0f) / 100.0f; s_valid = true; }
        else s_valid = false;
    }
    if (!s_valid) return false;
    *frac = s_frac;
    if (charging) *charging = false;            /* no charger-detect line on this board */
    return true;
}

int battery_port_state(void)
{
    return BAT_ON_BATTERY;                      /* no cable detection: never charging/full/plugged */
}

int battery_port_vbat_mv(void)
{
    return read_mv();
}

void battery_port_dump(void)
{
    int mv = read_mv();
    ESP_LOGI(TAG, "battery: %d mV, ~%d%%, %s calibration (divider %.2f:1, offset %.4f)",
             mv, lipo_pct(mv / 1000.0f), s_cali_ok ? "eFuse" : "rough", BAT_DIVIDER, BAT_CAL_OFFSET);
}

/* ---- no PMIC rails: the calls main.c/director.c make must exist --------- */
bool battery_port_set_rail(const char *name, bool on) { (void)name; (void)on; return false; }
void battery_port_pin_rail(const char *name) { (void)name; }
void battery_port_trim_rails(void) { }

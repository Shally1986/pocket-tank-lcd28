/* display_port_st7789.c — Waveshare ESP32-S3-Touch-LCD-2.8-V2 (LCD28) panel:
 * a 240x320 ST7789T over SPI3 (80 MHz, SCLK40/MOSI45/DC41/CS42/RST39) plus a
 * GPIO backlight on 5, dimmed by LEDC. The tank renders the landscape
 * 448x368 RGB565 frame (common/render.c); the panel is a portrait 240x320
 * ST7789, turned landscape (320x240) after init by swap_xy + mirror, and the
 * frame is downscaled to FIT (292x240, nearest-neighbour with half-pixel
 * centres) into its middle — the 14 px side bars are black. Colours are the
 * tank's RGB565 words VERBATIM on the wire (little-endian byte order, low
 * byte first) — the 16-bit ST7789 reads exactly what the bytes say; the
 * MADCTL BGR bit (0x08, from the dev config's rgb_endian) carries the
 * red<->blue pair. Bench 2026-10-09 (phase A/B bars on the glass): this
 * panel's inversion polarity is REVERSED — the datasheet's "INVOFF" (0x20)
 * INVERTS every colour on this glass (W read black), while "INVON" (0x21)
 * is the normal state. The frame goes out unswapped, and the port sends
 * 0x21 (invert_color(true)) right after init and at wake.
 *
 * The main I2C bus (port 0, SCL10/SDA11 — the RTC and the IMU live there) is
 * this port's, handed out as board_i2c_bus() (main.c). */
#include "sdkconfig.h"
#include "display_port.h"
#include "board_pins.h"
#include "tank.h"
#include "Vernon_ST7789T/Vernon_ST7789T.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "st7789";
#define LCD_HOST SPI3_HOST
#define PANEL_PW 320                              /* the landscape panel: 320 x 240 */
#define PANEL_PH 240
#define STRIPE_ROWS 32                            /* panel rows per DMA transfer */
#define STRIPE_BYTES (PANEL_PW * STRIPE_ROWS * 2) /* one stripe buffer (a window's rows fit in the same bytes) */
/* the 448x368 tank frame on the 320x240 glass, at full width (stretch, chosen
 * 2026-10-09): the window is the panel - WIN_W = PANEL_PW, no side bars
 * (WIN_X = 0; FIT was 292x240 with 14 px bars). The picture runs 9.6% wider:
 * fish read longer, and display_port_panel_to_tank shares the window's X gain
 * below, so touch stays true to the picture. */
#define WIN_W PANEL_PW
#define WIN_H 240
#define WIN_X 0
#define LEDC_MAX_DUTY13 ((1 << (LEDC_TIMER_13_BIT)) - 1)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;            /* kept for DCS writes (deep standby, M6) */
static uint16_t *s_stripe[2];                     /* DMA-capable; ping-pong */
static SemaphoreHandle_t s_stripe_free;
static int64_t s_prof_wait, s_prof_send;
static bool s_inverted;                           /* 180-degree flip, done in the window fill */
static bool s_clear;                              /* a reset panel's memory is not black (init, wake) */
static uint8_t s_brightness = 0xFF;               /* full, as the panel's own 0x51 would be on the AMOLEDs */
static i2c_master_bus_handle_t s_i2c;
static ledc_channel_config_t s_bl;
/* which source frame px answers each panel px (stretch window, half-pixel centres) */
static uint16_t s_row[WIN_H], s_col[WIN_W];

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx) {
    (void)io; (void)ev; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_stripe_free, &hp);
    return hp == pdTRUE;
}

static inline void stripe_take(void) {
    int64_t t = esp_timer_get_time();
    xSemaphoreTake(s_stripe_free, portMAX_DELAY);
    s_prof_wait += esp_timer_get_time() - t;
}
void display_port_flush_prof(int64_t *wait_us, int64_t *send_us) { *wait_us = s_prof_wait; *send_us = s_prof_send; s_prof_wait = s_prof_send = 0; }

static void send(int x0, int y0, int w, int n, const uint16_t *stripe) {
    int64_t t = esp_timer_get_time();
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + w, y0 + n, stripe);
    s_prof_send += esp_timer_get_time() - t;
    if (err != ESP_OK) {
        static int logged;
        if (logged++ < 3) ESP_LOGE(TAG, "draw_bitmap y0=%d: %s", y0, esp_err_to_name(err));
    }
}

/* one window row of the frame into the stripe, byte-swapped for the panel.
 * Flipped (s_inverted): the same window turned 180 degrees. */
static void fill_row(const uint16_t *fb, int j, uint16_t *out) {
    if (!s_inverted) {
        const uint16_t *row = fb + (size_t)s_row[j] * TANK_W;
        for (int i = 0; i < WIN_W; i++) out[i] = row[s_col[i]];      /* native 565 words, low byte first on the wire */
    } else {
        const uint16_t *row = fb + (size_t)(TANK_H - 1 - s_row[j]) * TANK_W + (TANK_W - 1);
        for (int i = 0; i < WIN_W; i++) out[i] = row[-s_col[i]];
    }
}

static void panel_black(int *cur) {               /* the whole panel once: a reset panel's memory is not black */
    for (int y0 = 0; y0 < PANEL_PH; y0 += STRIPE_ROWS) {
        int n = PANEL_PH - y0 < STRIPE_ROWS ? PANEL_PH - y0 : STRIPE_ROWS;
        stripe_take();
        memset(s_stripe[*cur], 0, (size_t)PANEL_PW * n * 2);
        send(0, y0, PANEL_PW, n, s_stripe[*cur]);
        *cur ^= 1;
    }
}

/* the picture's way up: the vendor driver's init leaves MADCTL at 0x00 (native
 * portrait) and carries the BGR bit from the dev config; the swap and the
 * mirror write MADCTL again (the BGR bit rides along); the wire itself is
 * native RGB565 words, low byte first (2026-10-09 colour bench).
 * Calibrated on the bench (2026-10-09): swap_xy + mirror(false,false) =
 * MADCTL 0x28 (0x20 MV: row/column exchange -> the 320x240 landscape window,
 * 0x08 BGR) reads upright. Bench reads along the way: mirror(true,false) (0x68)
 * flipped top/bottom; mirror(true,true) (0xE8) flipped both axes (the 180 wrong
 * state — same relation as the vendor's LVGL ROT_90 -> ROT_270). */
static void panel_upright(void) {
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, false);
}

static void dcs(uint8_t cmd, const uint8_t *param, size_t n) {
    if (s_io) esp_lcd_panel_io_tx_param(s_io, cmd, param, n);
}

static void bl_off(void) { ledc_set_duty(s_bl.speed_mode, s_bl.channel, 0); ledc_update_duty(s_bl.speed_mode, s_bl.channel); }

void display_port_set_brightness(uint8_t level) {
    s_brightness = level;
    uint8_t light = (uint16_t)((uint16_t)level * 100 + 127) / 255;      /* 0..255 -> the vendor's 0..100 scale */
    uint16_t duty = light == 0 ? 0 : (uint16_t)(LEDC_MAX_DUTY13 - 81 * (100 - light));
    ledc_set_duty(s_bl.speed_mode, s_bl.channel, duty);
    ledc_update_duty(s_bl.speed_mode, s_bl.channel);
}
uint8_t display_port_brightness(void) { return s_brightness; }

bool board_is_round(void) { return false; }
bool board_is_watch(void) { return false; }
bool board_has_expander(void) { return false; }   /* the LCD28 keeps its panel resets on GPIOs (none to scale) */
int  board_pwr_sense_pin(void) { return -1; }     /* no SYS_OUT line: the PWR key (GPIO6) is polled, not sensed (M3/M6) */
int  board_key_wake_pin(void) { return L28_PIN_KEY; }   /* M6: the PWR key (GPIO6, active low) is the deep-sleep ext1 wake */
void display_port_frame_origin(int *px, int *py) { *px = 0; *py = 0; }
void display_port_set_inverted(bool inverted) { s_inverted = inverted; }

int  display_port_view(void) { return DISPLAY_VIEW_FIT; }
void display_port_set_view(int view) { (void)view; }   /* one FIT view on this board */
void display_port_panel_to_tank(int px, int py, float *tx, float *ty) {
    *tx = (float)((px - WIN_X) * TANK_W) / WIN_W;
    *ty = (float)(py * TANK_H) / WIN_H;
}

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c; }

/* M1 keeps the deep-sleep paths no-ops in effect (main.c uses them only where
 * board_pwr_sense_pin() >= 0, which the LCD28 is not, until the PWR key joins
 * the picture in M6): the panel here has a GPIO power latch of its own and a
 * plain cell meter, so the night's question is the latch-drop's, not the
 * panel's. The functions still exist for the M6 work. */
void display_port_deep_sleep_pins(bool tp_awake_high) { (void)tp_awake_high; }
void display_port_deep_sleep_bus(void) { }
void display_port_deep_standby(void) { dcs(0x10, NULL, 0); }   /* sleep-in, when the night runs on the panel's own draw */

bool display_port_init(void) {
    i2c_master_bus_config_t bus = { .i2c_port = I2C_NUM_0, .sda_io_num = L28_PIN_I2C_SDA, .scl_io_num = L28_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));
    ESP_LOGI(TAG, "board: LCD28 (ST7789T 240x320 / CST328) - the main I2C bus (SCL%d, SDA%d) is this port's", L28_PIN_I2C_SCL, L28_PIN_I2C_SDA);

    for (int i = 0; i < 2; i++) {
        s_stripe[i] = heap_caps_malloc(STRIPE_BYTES, MALLOC_CAP_DMA);
        if (!s_stripe[i]) { ESP_LOGE(TAG, "no DMA memory for %d stripes", (int)STRIPE_BYTES); return false; }
    }
    s_stripe_free = xSemaphoreCreateCounting(2, 2);
    for (int j = 0; j < WIN_H; j++) s_row[j] = (uint16_t)(((j << 1) + 1) * TANK_H / (WIN_H << 1));   /* half-pixel centres */
    for (int i = 0; i < WIN_W; i++) s_col[i] = (uint16_t)(((i << 1) + 1) * TANK_W / (WIN_W << 1));

    const spi_bus_config_t spi = {
        .sclk_io_num = L28_PIN_LCD_SCLK, .mosi_io_num = L28_PIN_LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = STRIPE_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &spi, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = L28_PIN_LCD_DC, .cs_gpio_num = L28_PIN_LCD_CS, .pclk_hz = 80 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
        .on_color_trans_done = on_trans_done, .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));
    const esp_lcd_panel_dev_st7789t_config_t dev = { .reset_gpio_num = L28_PIN_LCD_RST,
        .rgb_endian = LCD_RGB_ENDIAN_BGR, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789t(s_io, &dev, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* 2026-10-09 colour bench (phase A/B bars on the glass): this panel's
     * inversion polarity is REVERSED — 0x20 (datasheet INVOFF) INVERTS every
     * colour (W read black), 0x21 (INVON) is the normal state. The frame goes
     * out NATIVE (unswapped) in fill_row. Fixes: 0x21 here and at wake,
     * no byte swap. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    panel_upright();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    gpio_config_t bk = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << L28_PIN_LCD_BL };
    ESP_ERROR_CHECK(gpio_config(&bk));
    ledc_timer_config_t t = { .duty_resolution = LEDC_TIMER_13_BIT, .freq_hz = 5000,
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0, .clk_cfg = LEDC_AUTO_CLK };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    s_bl.channel = LEDC_CHANNEL_0; s_bl.duty = 0; s_bl.gpio_num = L28_PIN_LCD_BL;
    s_bl.speed_mode = LEDC_LOW_SPEED_MODE; s_bl.timer_sel = LEDC_TIMER_0;
    ESP_ERROR_CHECK(ledc_channel_config(&s_bl));
    display_port_set_brightness(s_brightness);

    s_clear = true;                                /* a reset panel's memory is not black: the first flush pays it */
    ESP_LOGI(TAG, "panel up: %dx%d landscape, the %dx%d tank frame at full width (stretch, %dx%d)", PANEL_PW, PANEL_PH,
             TANK_W, TANK_H, WIN_W, WIN_H);
    return true;
}

/* sleep: display off, backlight out. No sleep-in yet (M6); the wake re-inits
 * the panel all the same. */
void display_port_sleep(void) {
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
    bl_off();
}

void display_port_wake(void) {
    if (!s_panel) return;
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));   /* 0x21 = normal on this glass (reversed polarity) */
    panel_upright();
    display_port_set_brightness(s_brightness);     /* the init left it at full */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    s_clear = true;
}

void display_port_flush(const uint16_t *fb) {
    if (!s_panel) return;
    int cur = 0;
    if (s_clear) { s_clear = false; panel_black(&cur); }
    for (int y0 = 0; y0 < WIN_H; y0 += STRIPE_ROWS) {
        int n = WIN_H - y0 < STRIPE_ROWS ? WIN_H - y0 : STRIPE_ROWS;
        stripe_take();
        uint16_t *stripe = s_stripe[cur];
        for (int r = 0; r < n; r++) fill_row(fb, y0 + r, stripe + r * WIN_W);
        send(WIN_X, y0, WIN_W, n, stripe);
        cur ^= 1;
    }
}
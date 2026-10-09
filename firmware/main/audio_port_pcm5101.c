/* audio_port_pcm5101.c - I2S -> PCM5101 DAC -> the 2.8" board's line out /
 * headphone / speaker (whatever is connected to the header).
 * See audio_port.h. Pins from the board's main/ (BSP_I2S_* in the vendor
 * Audio_Driver/PCM5101.h): BCLK 48, WS/LRCK 38, DOUT 47, and NO MCLK - the
 * vendor wires GPIO_NUM_NC and the PCM5101 derives its system clock from
 * BCK/LCK (auto clock detect). There is no codec (no I2C, no register
 * file), no amp (no NS4150B / GPIO ctrl) and no PMIC rail (the DAC is
 * powered straight from the always-on 3V3). So none of codec_port_*,
 * battery_port_set_rail or an amp pin appear here.
 *
 * Power (docs/AUDIO.md section 3, adapted): idle = I2S stopped, nothing
 * else to do - the DAC itself is always powered but consumes ~nothing
 * unclocked. The first cue after silence brings I2S up and writes ~20 ms of
 * zeros (the PCM5101's own pop suppression - its thump on clock start);
 * 2 s after the last voice ends it all goes down again.
 * audio_port_sleep() forces that before a drowse. */
#include "audio_port.h"
#include "audio.h"
#include "board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "audio";

#define PIN_I2S_BCLK  48
#define PIN_I2S_WS    38
#define PIN_I2S_DOUT  47
/* no MCLK (vendor uses NC; the PCM5101 auto-detects its clock from BCK/LCK),
   no amp GPIO, no codec I2C, no ALDO rail */
#define BLOCK         160      /* 10 ms at 16 kHz */
#define IDLE_US       (2 * 1000000LL)

extern const uint8_t _binary_sounds_bin_start[];
extern const uint8_t _binary_sounds_bin_end[];

static i2s_chan_handle_t  s_tx;
static SemaphoreHandle_t  s_mx;
static TaskHandle_t       s_task;
static bool s_ok, s_up;
static volatile bool s_sleep_req, s_warm_req;
static int64_t s_quiet_since;
static int s_volume = 2;
static int16_t s_buf[BLOCK];
/* the DAC's own start-up settle (its pop/thump while the analog output
   biases): 20 ms of zeros is the plan's default, tunable live (director
   `snd settle <ms> <ignored> <idle s>`; the amp_ms knob is ignored - there
   is no amp) */
static int s_settle_ms = 20;
static int64_t s_idle_us = 5 * 1000000LL;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Deep sleep (2026-09-16, the night the tank died): the I2S lines are the
 * ESP's outputs into ICs that stay powered on VCC3V3 all night (the
 * PCM5101). In deep sleep an un-held digital pad is neither driven nor
 * pulled - and esp-idf isolates the REST of the digital pads only once
 * digital hold is on (sleep_gpio.c: without it "the bottom current of deep
 * sleep will be higher than light sleep"). So before esp_deep_sleep_start:
 * each of these a plain GPIO, driven low, held. The DAC is already down
 * (audio_port_sleep) and the wake is a reboot; audio_port_init releases the
 * holds before the drivers claim the pins again. No MCLK / amp pins to add
 * on this board. */
#define QUIET_N 3
static void quiet_pins(gpio_num_t p[QUIET_N]) {
    p[0] = PIN_I2S_BCLK; p[1] = PIN_I2S_WS; p[2] = PIN_I2S_DOUT;
}
void audio_port_deep_sleep_pins(void) {
    gpio_num_t QUIET_PINS[QUIET_N]; quiet_pins(QUIET_PINS);
    for (size_t i = 0; i < QUIET_N; i++) {
        gpio_num_t p = QUIET_PINS[i];
        gpio_reset_pin(p);                        /* off the I2S matrix routing, a GPIO again */
        gpio_set_direction(p, GPIO_MODE_OUTPUT);
        gpio_set_level(p, 0);
        gpio_hold_en(p);
    }
}
static void release_pins(void) {
    gpio_num_t QUIET_PINS[QUIET_N]; quiet_pins(QUIET_PINS);
    for (size_t i = 0; i < QUIET_N; i++) gpio_hold_dis(QUIET_PINS[i]);
}

static void write_silence(int ms) {
    memset(s_buf, 0, sizeof s_buf);
    size_t w;
    for (int i = 0; i < ms / 10; i++) i2s_channel_write(s_tx, s_buf, sizeof s_buf, &w, 100);
}

/* power up: clocks on -> zeros (the DAC's own pop suppression) */
static void bring_up(void) {
    int64_t t0 = esp_timer_get_time();
    i2s_channel_enable(s_tx);                 /* BCK/LCK running before the DAC biases */
    write_silence(s_settle_ms);
    s_up = true; s_quiet_since = 0;
    s_warm_req = false;                         /* every prewarm asked for while this came up
                                                   (the tank asks each frame it is handled) is
                                                   answered; see the ES8311 port for the history */
    ESP_LOGI(TAG, "up in %lld ms (DAC, no codec/amp)", (esp_timer_get_time() - t0) / 1000);
}
/* power down: zeros -> clocks off; the DAC has nothing else to do */
static void bring_down(void) {
    write_silence(10);
    i2s_channel_disable(s_tx);
    s_up = false;
    ESP_LOGI(TAG, "down (idle)");
}

static void player(void *arg) {
    (void)arg;
    for (;;) {
        if (!s_up) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (s_sleep_req) { s_sleep_req = false; continue; }
            xSemaphoreTake(s_mx, portMAX_DELAY);
            bool any = audio_active() || s_warm_req;
            xSemaphoreGive(s_mx);
            s_warm_req = false;
            if (!any) continue;
            bring_up();
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        int live = audio_render(s_buf, BLOCK);
        xSemaphoreGive(s_mx);
        size_t w;
        i2s_channel_write(s_tx, s_buf, sizeof s_buf, &w, 100);   /* blocks ~10 ms: the DMA paces us */
        int64_t now = esp_timer_get_time();
        if (live) s_quiet_since = 0;
        else if (!s_quiet_since) s_quiet_since = now;
        if (s_sleep_req || (s_idle_us && s_quiet_since && now - s_quiet_since > s_idle_us)) {
            bool sleeping = s_sleep_req;
            if (sleeping) { xSemaphoreTake(s_mx, portMAX_DELAY); audio_stop_all(); xSemaphoreGive(s_mx); }
            bring_down();
            s_sleep_req = false;
            if (sleeping) { s_warm_req = false; ulTaskNotifyTake(pdTRUE, 0); }   /* down for a sleep stays down: no standing request, no pending nudge */
        }
    }
}

static void load_volume(void) {
    nvs_handle_t h; uint8_t v;
    if (nvs_open("tank", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, "snd", &v) == ESP_OK && v <= 2) s_volume = v;
    nvs_close(h);
}

bool audio_port_init(i2c_master_bus_handle_t bus) {
    (void)bus;                                 /* the PCM5101 has no I2C - nothing to probe */
    release_pins();                            /* a deep-sleep wake is a boot: drop the holds first */
    size_t bank_bytes = (size_t)(_binary_sounds_bin_end - _binary_sounds_bin_start);
    if (bank_bytes != SND_BANK_BYTES) { ESP_LOGW(TAG, "bank is %u bytes, sounds.h says %u: rebuild (tools/make_sounds.py build) - silent", (unsigned)bank_bytes, (unsigned)SND_BANK_BYTES); return false; }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4; cc.dma_frame_num = BLOCK;
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) { ESP_LOGE(TAG, "no I2S channel"); return false; }
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SND_RATE),              /* no MCLK: the PCM5101 takes its clock from BCK/LCK */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS, .dout = PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED,
                      .invert_flags = { 0 } },
    };
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) { ESP_LOGE(TAG, "I2S std init failed"); return false; }
    audio_init((const int16_t *)_binary_sounds_bin_start, SND_BANK_SAMPLES);   /* flash-mapped: no RAM */
    load_volume(); audio_set_volume(s_volume);
    s_mx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(player, "audio", 4096, NULL, 6, &s_task, 1);
    s_ok = true;
    int present = 0; for (int i = 0; i < SND_COUNT; i++) present += SND_CUES[i].n_var > 0;
    ESP_LOGI(TAG, "PCM5101 DAC: %d of %d cues, bank %u KB in flash, volume %d; warm while handled (touch / IMU), down %lld s after", present, SND_COUNT, (unsigned)(SND_BANK_BYTES / 1024), s_volume, s_idle_us / 1000000);
    return true;
}

void audio_port_play(int cue, int pitch_q8) {
    if (!s_ok) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool started = audio_play(cue, pitch_q8, now_ms());
    xSemaphoreGive(s_mx);
    if (started) xTaskNotifyGive(s_task);
}
void audio_port_prewarm(void) {
    if (!s_ok) return;
    s_quiet_since = 0;                          /* activity: the idle clock restarts */
    if (s_up) return;
    s_warm_req = true; xTaskNotifyGive(s_task);
}
void audio_port_stop(int cue) {
    if (!s_ok) return;
    xSemaphoreTake(s_mx, portMAX_DELAY); audio_stop(cue); xSemaphoreGive(s_mx);
}
void audio_port_set_volume(int level) {
    s_volume = level < 0 ? 0 : level > 2 ? 2 : level;
    if (s_ok) { xSemaphoreTake(s_mx, portMAX_DELAY); audio_set_volume(s_volume); xSemaphoreGive(s_mx); }
    nvs_handle_t h;
    if (nvs_open("tank", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "snd", (uint8_t)s_volume); nvs_commit(h); nvs_close(h); }
    ESP_LOGI(TAG, "volume %s", s_volume == 0 ? "off" : s_volume == 1 ? "quiet" : "normal");
}
int  audio_port_volume(void) { return s_volume; }
void audio_port_set_night(bool night) {
    if (!s_ok) return;
    xSemaphoreTake(s_mx, portMAX_DELAY); audio_set_night(night); xSemaphoreGive(s_mx);
}
void audio_port_sleep(void) {
    if (!s_ok || !s_up) return;
    s_sleep_req = true; xTaskNotifyGive(s_task);
    for (int i = 0; i < 50 && s_up; i++) vTaskDelay(pdMS_TO_TICKS(10));   /* one block + the down sequence */
    if (s_up) ESP_LOGW(TAG, "still up at sleep: forcing the channel off");
    i2s_channel_disable(s_tx);
}
void audio_port_tune(int dac_ms, int amp_ms, int idle_s) {
    if (dac_ms >= 0) s_settle_ms = dac_ms;
    (void)amp_ms;                               /* no amp on this board */
    if (idle_s >= 0) s_idle_us = (int64_t)idle_s * 1000000;
    ESP_LOGI(TAG, "settle %d ms (no codec/amp), idle %lld s (0 = warm while awake)", s_settle_ms, s_idle_us / 1000000);
}
bool audio_port_up(void) { return s_up; }
const char *audio_port_state(void) { return !s_ok ? "absent" : s_up ? "up" : "idle"; }
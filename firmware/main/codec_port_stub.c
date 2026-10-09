/* codec_port_stub.c — the ESP32-S3-Touch-LCD-2.8-V2 has no audio codec
 * (the PCM5101 is a DAC only, no I2C, no register file). main.c calls the
 * codec_port API unconditionally, so this board gets a no-op implementation;
 * audio_port_pcm5101.c must never call into it. */
#include "codec_port.h"
#include "esp_log.h"

static const char *TAG = "codec";

bool codec_port_init(i2c_master_bus_handle_t bus) { (void)bus; ESP_LOGI(TAG, "no codec on this board (stub)"); return false; }
bool codec_port_present(void) { return false; }
bool codec_port_up(void) { return false; }
void codec_port_settled(void) { }
void codec_port_down(void) { }
void codec_port_dump(void) { }
bool codec_port_mic_adc_down(i2c_master_bus_handle_t bus) { (void)bus; return false; }

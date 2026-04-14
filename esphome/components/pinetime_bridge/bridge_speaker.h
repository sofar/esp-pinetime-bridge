#pragma once
#include "esphome/core/log.h"
#include "esphome/components/i2c/i2c.h"
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "event_tones.h"

namespace esphome {
namespace bridge_speaker {

static const char *const TAG = "bridge_speaker";

// ES8311 codec init over I2C. Call AFTER the I2C bus is up and the I2S peripheral
// is generating MCLK (post bridge_speaker::init()). 16kHz, 16-bit, I2S slave.
static bool init_codec(esphome::i2c::I2CBus *bus, uint8_t addr = 0x18) {
  auto w = [&](uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return bus->write(addr, buf, 2, true) == esphome::i2c::ERROR_OK;
  };

  // Probe address — Waveshare board uses 0x18 (CE pin tied low). Fall back to 0x19.
  uint8_t probe_reg = 0xFD, probe_val = 0;
  if (bus->write(addr, &probe_reg, 1, false) != esphome::i2c::ERROR_OK ||
      bus->read(addr, &probe_val, 1) != esphome::i2c::ERROR_OK) {
    addr = 0x19;
    if (bus->write(addr, &probe_reg, 1, false) != esphome::i2c::ERROR_OK ||
        bus->read(addr, &probe_val, 1) != esphome::i2c::ERROR_OK) {
      ESP_LOGE(TAG, "ES8311 not found at 0x18 or 0x19");
      return false;
    }
  }
  ESP_LOGI(TAG, "ES8311 found at 0x%02X (chip id reg 0xFD=0x%02X)", addr, probe_val);

  // Standard Espressif ES8311 init: 16kHz, 16-bit, I2S slave, MCLK from pin, DAC unmuted.
  bool ok = true;
  ok &= w(0x00, 0x1F);  // reset
  vTaskDelay(pdMS_TO_TICKS(10));
  ok &= w(0x45, 0x00);
  ok &= w(0x01, 0x30);  // clock manager: bclk_inv=0, mclk_src=MCLK pin
  ok &= w(0x02, 0x00);  // clk div
  ok &= w(0x03, 0x10);  // FS coeff
  ok &= w(0x16, 0x24);  // ADC filter defaults
  ok &= w(0x04, 0x10);  // DAC osr
  ok &= w(0x05, 0x00);  // adc/dac clock divider
  ok &= w(0x06, 0x03);  // sclk settings (64 sclk per frame)
  ok &= w(0x07, 0x00);
  ok &= w(0x08, 0xFF);
  ok &= w(0x00, 0x80);  // power up, slave mode
  ok &= w(0x0D, 0x01);
  ok &= w(0x01, 0x3F);  // all clocks on
  ok &= w(0x14, 0x1A);  // analog PGA 0dB
  ok &= w(0x12, 0x00);  // ADC power
  ok &= w(0x13, 0x10);
  ok &= w(0x10, 0x1F);  // VMID ramp
  ok &= w(0x11, 0x7F);  // analog power up
  ok &= w(0x00, 0x80);  // stay powered, slave
  ok &= w(0x0E, 0x02);
  ok &= w(0x0F, 0x44);
  ok &= w(0x15, 0x00);
  ok &= w(0x1B, 0x0A);
  ok &= w(0x1C, 0x6A);
  ok &= w(0x37, 0x08);  // DAC ramp rate
  ok &= w(0x44, 0x08);  // ADC→DAC loopback off
  ok &= w(0x17, 0xBF);  // ADC vol (unused but standard)
  // Format: 16-bit I2S
  ok &= w(0x09, 0x0C);  // SDP in: 16-bit I2S
  ok &= w(0x0A, 0x0C);  // SDP out: 16-bit I2S
  // DAC output
  ok &= w(0x32, 0xBF);  // DAC volume ≈ 0 dB
  ok &= w(0x31, 0x00);  // DAC unmute
  // GPIO reg — leave default; PA on this board is powered from PMIC, not codec GPIO.

  if (!ok) {
    ESP_LOGE(TAG, "ES8311 init had I2C write failures");
    return false;
  }
  ESP_LOGI(TAG, "ES8311 codec initialized");
  return true;
}

static i2s_chan_handle_t tx_handle_ = NULL;
static SemaphoreHandle_t mutex_ = NULL;
static bool ready_ = false;
static int pending_tone_id_ = -1;
static uint8_t volume_ = 50;  // 0-100%, default 50%

static void init() {
  // Enable PA
  gpio_config_t io_conf = {};
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << GPIO_NUM_46);
  gpio_config(&io_conf);
  gpio_set_level(GPIO_NUM_46, 1);

  // I2S config — Waveshare ESP-IDF example settings
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, NULL));

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = GPIO_NUM_16,
      .bclk = GPIO_NUM_9,
      .ws = GPIO_NUM_45,
      .dout = GPIO_NUM_8,
      .din = GPIO_NUM_10,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));

  mutex_ = xSemaphoreCreateMutex();
  ready_ = true;
  ESP_LOGI(TAG, "Speaker ready");
}

// Play mono 16-bit 16kHz PCM (converts to stereo for I2S)
static void play_mono_pcm(const uint8_t *data, size_t len) {
  if (!ready_ || !data || len == 0) return;
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE) {
    ESP_LOGW(TAG, "play skipped: mutex busy");
    return;
  }

  size_t num_samples = len / 2;
  int16_t *stereo = (int16_t *)heap_caps_malloc(num_samples * 4, MALLOC_CAP_DEFAULT);
  if (!stereo) {
    ESP_LOGE(TAG, "malloc failed for %d bytes", num_samples * 4);
    xSemaphoreGive(mutex_);
    return;
  }

  const int16_t *mono = (const int16_t *)data;
  float gain = (float)volume_ / 100.0f;
  for (size_t i = 0; i < num_samples; i++) {
    int16_t scaled = (int16_t)((float)mono[i] * gain);
    stereo[i * 2] = scaled;
    stereo[i * 2 + 1] = scaled;
  }

  size_t bytes_written = 0;
  esp_err_t err = i2s_channel_write(tx_handle_, stereo, num_samples * 4, &bytes_written, pdMS_TO_TICKS(2000));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "i2s_write err=%d wrote=%u/%u", err, (unsigned)bytes_written, (unsigned)(num_samples * 4));
  }

  free(stereo);
  xSemaphoreGive(mutex_);
}

// Play an event tone (non-blocking, spawns task)
static void play_task_(void *arg) {
  int tone_id = (int)(intptr_t)arg;
  auto tone = get_event_tone(static_cast<EventTone>(tone_id));
  play_mono_pcm(tone.data, tone.len);
  // Small silence after tone to let DMA flush
  vTaskDelay(pdMS_TO_TICKS(100));
  vTaskDelete(NULL);
}

static void play_event_tone(int tone_id) {
  if (!ready_) {
    ESP_LOGW(TAG, "play_event_tone(%d) ignored: not ready", tone_id);
    return;
  }
  ESP_LOGI(TAG, "play_event_tone(%d)", tone_id);
  xTaskCreate(play_task_, "spk", 4096, (void *)(intptr_t)tone_id, 5, NULL);
}

static uint8_t get_volume() { return volume_; }

static void set_volume(uint8_t vol) {
  if (vol > 100) vol = 100;
  volume_ = vol;
  ESP_LOGI(TAG, "Volume: %u%%", volume_);
}

static void volume_up() {
  set_volume(volume_ <= 90 ? volume_ + 10 : 100);
}

static void volume_down() {
  set_volume(volume_ >= 10 ? volume_ - 10 : 0);
}

}  // namespace bridge_speaker
}  // namespace esphome

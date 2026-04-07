#pragma once
#include "esphome/core/log.h"
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "event_tones.h"

namespace esphome {
namespace bridge_speaker {

static const char *const TAG = "bridge_speaker";

static i2s_chan_handle_t tx_handle_ = NULL;
static SemaphoreHandle_t mutex_ = NULL;
static bool ready_ = false;
static int pending_tone_id_ = -1;

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
  if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return;

  size_t num_samples = len / 2;
  int16_t *stereo = (int16_t *)heap_caps_malloc(num_samples * 4, MALLOC_CAP_DEFAULT);
  if (!stereo) {
    ESP_LOGE(TAG, "malloc failed for %d bytes", num_samples * 4);
    xSemaphoreGive(mutex_);
    return;
  }

  const int16_t *mono = (const int16_t *)data;
  for (size_t i = 0; i < num_samples; i++) {
    stereo[i * 2] = mono[i];
    stereo[i * 2 + 1] = mono[i];
  }

  size_t bytes_written = 0;
  i2s_channel_write(tx_handle_, stereo, num_samples * 4, &bytes_written, portMAX_DELAY);

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
  if (!ready_) return;
  xTaskCreate(play_task_, "spk", 4096, (void *)(intptr_t)tone_id, 5, NULL);
}

}  // namespace bridge_speaker
}  // namespace esphome

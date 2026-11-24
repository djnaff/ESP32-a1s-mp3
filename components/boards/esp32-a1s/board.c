// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file board.c
 * @brief Board implementation for the Ai-Thinker ESP32-A1S Audio Kit V2.2
 *
 * The board carries an ESP32-WROVER (8 MB PSRAM) and an ES8388 codec on the
 * A1S module. The codec is reachable over I2C (SDA 33, SCL 32, address 0x10)
 * and is fed by I2S (MCLK 0, BCK 27, WS 25, DOUT 26); its LOUT/ROUT outputs
 * drive a pair of NS4150 class-D amplifiers whose shared enable line is
 * GPIO 21.
 *
 * The upstream ES8388 driver does the codec register work, so this file only
 * brings up the I2C bus, registers the driver, restores the saved volume and
 * gates the power amplifiers with playback.
 */

#include "iot_board.h"

#include "dac.h"
#include "dac_es8388.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "playback_events.h"
#include "settings.h"

static const char TAG[] = "ESP32A1S";

static bool s_board_initialized = false;
static i2c_master_bus_handle_t s_i2c_dac_bus_handle = NULL;
static volatile bool s_playing = false;

static void on_playback_event(playback_source_t source, playback_event_t event,
                              const playback_event_data_t *data,
                              void *user_data);

// The NS4150s are the only muteable stage: the ES8388 keeps its own outputs
// enabled, so the amplifiers simply follow playback.
static void pa_update(void) {
#if CONFIG_DAC_ENABLE_GPIO >= 0
  gpio_set_level(BOARD_PA_ENABLE_GPIO, s_playing ? 1 : 0);
#endif
}

static esp_err_t init_control_gpios(void) {
#if CONFIG_DAC_ENABLE_GPIO >= 0
  gpio_config_t pa_conf = {
      .pin_bit_mask = (1ULL << BOARD_PA_ENABLE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&pa_conf), TAG,
                      "Failed to configure PA enable GPIO");
  // Start silent; the first PLAYING event turns the amps on.
  gpio_set_level(BOARD_PA_ENABLE_GPIO, 0);
  ESP_LOGI(TAG, "PA enable GPIO %d initialized (off)", BOARD_PA_ENABLE_GPIO);
#endif
  return ESP_OK;
}

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
  case BOARD_I2C_DAC_ID:
    return (board_res_handle_t)s_i2c_dac_bus_handle;
  default:
    return NULL;
  }
}

esp_err_t iot_board_init(void) {
  esp_err_t err = ESP_OK;

  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

  // A previous failed cleanup may still own a bus. Release it before retrying.
  err = iot_board_deinit();
  if (err != ESP_OK) {
    return err;
  }

  err = init_control_gpios();
  if (err != ESP_OK) {
    goto fail;
  }

  // Codec control bus
  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = BOARD_I2C_PORT,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  err = i2c_new_master_bus(&i2c_cfg, &s_i2c_dac_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize DAC I2C bus: %s",
             esp_err_to_name(err));
    goto fail;
  }
  ESP_LOGI(TAG, "DAC I2C bus %d initialized: sda=%d, scl=%d", BOARD_I2C_PORT,
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);

  dac_register(&dac_es8388_ops);
  err = dac_init(s_i2c_dac_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize ES8388: %s", esp_err_to_name(err));
    goto fail;
  }

  // The codec boots at 0 dB; restore what the user last set.
  float vol_db;
  if (ESP_OK == settings_get_volume(&vol_db)) {
    dac_set_volume(vol_db);
  }

  if (playback_events_register(on_playback_event, NULL) != 0) {
    ESP_LOGE(TAG, "Failed to register DAC playback listener");
    err = ESP_ERR_NO_MEM;
    goto fail;
  }

  s_board_initialized = true;
  ESP_LOGI(TAG, "%s initialized", BOARD_NAME);
  return ESP_OK;

fail:
  iot_board_deinit();
  return err;
}

esp_err_t iot_board_deinit(void) {
  // Also release resources acquired by an incomplete init.
  s_board_initialized = false;
  playback_events_unregister(on_playback_event);

  ESP_RETURN_ON_ERROR(dac_deinit(), TAG, "DAC cleanup failed");

  if (s_i2c_dac_bus_handle) {
    ESP_RETURN_ON_ERROR(i2c_del_master_bus(s_i2c_dac_bus_handle), TAG,
                        "I2C cleanup failed");
    s_i2c_dac_bus_handle = NULL;
  }

#if CONFIG_DAC_ENABLE_GPIO >= 0
  gpio_set_level(BOARD_PA_ENABLE_GPIO, 0);
#endif

  return ESP_OK;
}

static void on_playback_event(playback_source_t source, playback_event_t event,
                              const playback_event_data_t *data,
                              void *user_data) {
  (void)source;
  (void)data;
  (void)user_data;

  switch (event) {
  case PLAYBACK_EVENT_CONNECTED:
  case PLAYBACK_EVENT_PAUSED:
    s_playing = false;
    pa_update();
    dac_set_power_mode(DAC_POWER_STANDBY);
    break;
  case PLAYBACK_EVENT_PLAYING: {
    s_playing = true;
    pa_update();
    // The output starts after boot; make sure the first audio uses the saved
    // level rather than the codec's reset default.
    float vol_db;
    if (ESP_OK == settings_get_volume(&vol_db)) {
      dac_set_volume(vol_db);
    }
    dac_set_power_mode(DAC_POWER_ON);
    break;
  }
  case PLAYBACK_EVENT_DISCONNECTED:
    s_playing = false;
    pa_update();
    dac_set_power_mode(DAC_POWER_OFF);
    break;
  case PLAYBACK_EVENT_METADATA:
    break;
  }
}

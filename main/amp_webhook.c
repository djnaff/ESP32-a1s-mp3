// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file amp_webhook.c
 * @brief Power an external amplifier with Home Assistant webhooks.
 *
 * The NS4150s on the board are gated by board.c. This is for the other case:
 * an amplifier that lives outside the box and is switched by Home Assistant.
 * The board cannot see that switch, so it just POSTs to a pair of webhook
 * URLs when playback starts and when it has been quiet long enough that the
 * amplifier need not stay powered.
 *
 * playback_events listeners run on whichever task emitted the event (RTSP,
 * Bluetooth, the SD player). None of them may block on a TLS handshake, so
 * the listener only wakes a worker task, which owns all the HTTP and the
 * inactivity timer.
 */

#include "amp_webhook.h"

#include <stdbool.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "playback_events.h"
#include "sdkconfig.h"

static const char TAG[] = "amp_webhook";

#define HTTP_TIMEOUT_MS 4000
#define POST_ATTEMPTS   3
#define RETRY_DELAY_MS  5000
#define TASK_STACK      6144
#define TASK_PRIO       3

static SemaphoreHandle_t s_wake = NULL;
static TaskHandle_t s_task = NULL;

// Logical state of the external amplifier, as last commanded. Webhook
// failures are logged but do not hold up the next transition: an unreachable
// Home Assistant must not turn every pause into a retry storm.
static bool s_amp_on = false;
static bool s_off_timer_armed = false;
static int64_t s_off_at_us = 0;

// POST with no body, matching:
//   curl -k -X POST "<url>"
// Certificate verification is off because Home Assistant commonly serves a
// self-signed certificate here; CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is
// what makes that work in ESP-IDF.
static bool post_webhook(const char *url, const char *what) {
  const esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = HTTP_TIMEOUT_MS,
      .disable_auto_redirect = true,
      .skip_cert_common_name_check = true,
  };

  for (int attempt = 1; attempt <= POST_ATTEMPTS; attempt++) {
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      ESP_LOGE(TAG, "Amp %s: HTTP client init failed", what);
      return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status >= 200 && status < 300) {
      ESP_LOGI(TAG, "Amp %s webhook sent (HTTP %d)", what, status);
      return true;
    }

    ESP_LOGW(TAG, "Amp %s webhook failed: %s (HTTP %d), attempt %d/%d", what,
             err == ESP_OK ? "rejected" : esp_err_to_name(err), status, attempt,
             POST_ATTEMPTS);

    if (attempt < POST_ATTEMPTS) {
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
  }

  ESP_LOGE(TAG, "Amp %s webhook gave up", what);
  return false;
}

static void arm_off_timer(void) {
  s_off_timer_armed = true;
  s_off_at_us = esp_timer_get_time() +
                (int64_t)CONFIG_AMP_WEBHOOK_OFF_DELAY_S * 1000000LL;
}

static void amp_webhook_task(void *arg) {
  (void)arg;

  // A reset or an unplug while playing leaves the amplifier on. Normalise to
  // off at boot; the webhook is idempotent. Best effort, since the network may
  // not be reachable yet and playback does not depend on it.
  (void)post_webhook(CONFIG_AMP_WEBHOOK_OFF_URL, "off");
  s_amp_on = false;

  for (;;) {
    TickType_t wait = portMAX_DELAY;
    if (s_off_timer_armed) {
      int64_t remaining_us = s_off_at_us - esp_timer_get_time();
      wait = remaining_us <= 0 ? 0 : pdMS_TO_TICKS(remaining_us / 1000);
    }
    xSemaphoreTake(s_wake, wait);

    // Re-read the aggregate rather than trusting the event that woke us: a
    // burst of events collapses into one iteration, and BT/AirPlay/SD all
    // feed the same truth.
    bool playing = playback_events_any_playing();

    if (playing) {
      s_off_timer_armed = false;
      if (!s_amp_on) {
        s_amp_on = true;
        (void)post_webhook(CONFIG_AMP_WEBHOOK_ON_URL, "on");
      }
    } else if (s_amp_on) {
      if (!s_off_timer_armed) {
        arm_off_timer();
      } else if (esp_timer_get_time() >= s_off_at_us) {
        s_amp_on = false;
        s_off_timer_armed = false;
        (void)post_webhook(CONFIG_AMP_WEBHOOK_OFF_URL, "off");
      }
    }
  }
}

static void on_playback_event(playback_source_t source, playback_event_t event,
                              const playback_event_data_t *data,
                              void *user_data) {
  (void)source;
  (void)data;
  (void)user_data;

  if (event == PLAYBACK_EVENT_METADATA || s_wake == NULL) {
    return;
  }
  xSemaphoreGive(s_wake);
}

esp_err_t amp_webhook_init(void) {
  if (s_task != NULL) {
    return ESP_OK;
  }

  if (strlen(CONFIG_AMP_WEBHOOK_ON_URL) == 0 ||
      strlen(CONFIG_AMP_WEBHOOK_OFF_URL) == 0) {
    ESP_LOGW(TAG, "Amp webhooks enabled but a URL is empty; not starting");
    return ESP_ERR_INVALID_STATE;
  }

  s_wake = xSemaphoreCreateBinary();
  if (s_wake == NULL) {
    return ESP_ERR_NO_MEM;
  }

  if (playback_events_register(on_playback_event, NULL) != 0) {
    vSemaphoreDelete(s_wake);
    s_wake = NULL;
    return ESP_ERR_NO_MEM;
  }

  if (xTaskCreate(amp_webhook_task, "amp_hook", TASK_STACK, NULL, TASK_PRIO,
                  &s_task) != pdPASS) {
    playback_events_unregister(on_playback_event);
    vSemaphoreDelete(s_wake);
    s_wake = NULL;
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "External amp webhooks enabled (%d s off delay)",
           CONFIG_AMP_WEBHOOK_OFF_DELAY_S);
  return ESP_OK;
}

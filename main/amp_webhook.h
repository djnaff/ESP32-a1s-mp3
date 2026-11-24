// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "esp_err.h"

/**
 * Switch an external amplifier through Home Assistant webhooks.
 *
 * The amplifier is not wired to this board, so the firmware only reports
 * intent: it POSTs to CONFIG_AMP_WEBHOOK_ON_URL when playback starts and to
 * CONFIG_AMP_WEBHOOK_OFF_URL once playback has been stopped for
 * CONFIG_AMP_WEBHOOK_OFF_DELAY_S seconds. A pause shorter than that leaves
 * the amplifier on, so tracks with gaps do not power-cycle it.
 *
 * The webhook posts run on their own low-priority task; the playback event
 * callback only wakes it. Call once at startup, after playback_events is up.
 *
 * @return ESP_OK when the listener and worker task are running
 */
esp_err_t amp_webhook_init(void);

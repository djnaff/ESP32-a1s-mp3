// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "board_common.h"
#include "sdkconfig.h"

#define BOARD_NAME "ESP32-A1S AudioKit"
#define BOARD_DESCRIPTION \
  "Ai-Thinker ESP32-A1S Audio Kit V2.2 (ESP32-WROVER, ES8388 codec)"

// I2S configuration. The A1S module wires the codec's MCLK to GPIO0 (the
// BOOT strapping pin), so that pin is unavailable for buttons.
#define BOARD_I2S_BCK_GPIO CONFIG_I2S_BCK_IO
#define BOARD_I2S_WS_GPIO  CONFIG_I2S_WS_IO
#define BOARD_I2S_DO_GPIO  CONFIG_I2S_DO_IO
#define BOARD_I2S_GND_GPIO CONFIG_I2S_GND_IO
#define BOARD_I2S_VCC_GPIO CONFIG_I2S_VCC_IO

// SPDIF configuration
#define BOARD_SPDIF_BCK_GPIO CONFIG_SPDIF_BCK_IO
#define BOARD_SPDIF_WS_GPIO  CONFIG_SPDIF_WS_IO
#define BOARD_SPDIF_DO_GPIO  CONFIG_SPDIF_DO_IO

// LED configuration (green LED, active low)
#define BOARD_LED_STATUS_GPIO CONFIG_LED_STATUS_GPIO
#define BOARD_LED_ERROR_GPIO  CONFIG_LED_ERROR_GPIO
#define BOARD_LED_RGB_GPIO    CONFIG_LED_RGB_GPIO

// ES8388 control bus
#define BOARD_I2C_PORT     0
#define BOARD_I2C_SDA_GPIO CONFIG_DAC_I2C_SDA
#define BOARD_I2C_SCL_GPIO CONFIG_DAC_I2C_SCL

// NS4150 speaker amplifier enable (both channels share this line, active
// high). Follows playback events.
#define BOARD_PA_ENABLE_GPIO CONFIG_DAC_ENABLE_GPIO

// Battery monitoring
#define BOARD_BAT_CHANNEL CONFIG_BAT_CHANNEL

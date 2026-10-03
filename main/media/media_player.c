/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser media player on top of esp_player (esp-gmf).
 * See media_player.h for the architecture notes.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "app_lcd.h"
#include "media_lib_adapter.h"
#include "esp_extractor_defaults.h"
#include "esp_audio_dec_default.h"
#include "esp_video_dec_default.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_alc.h"
#include "esp_audio_render.h"
#include "esp_video_render.h"
#include "esp_video_render_backend.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_gmf_video_ppa.h"
#endif
#include "esp_gmf_video_color_convert.h"
#include "esp_player.h"
#include "esp_player_advance.h"
#include "esp_task_wdt.h"
#include "freertos/idf_additions.h"
#include "media/media_audio.h"
#include "media/media_player.h"
#include "media/pstats.h"

static const char *TAG = "media_player";

#define MEDIA_URL_MAX 256

/* PLACEHOLDER_FULL_FILE - will replace */

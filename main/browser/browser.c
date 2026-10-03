/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser: touch-driven text browser on the JD9165 1024x600 LCD.
 *
 * State machine (single FreeRTOS task, core 1; the camera stream task is
 * stopped before this task owns the screen, see docs/ETHERNET_BROWSER.md):
 *
 *   SPLASH ──(net ready / tap)──▶ HOME (query + keyboard)
 *   HOME ──GO──▶ LOADING ──▶ RESULTS ──tap──▶ PAGELOAD ──▶ PAGE
 *   RESULTS/PAGE ──nav bar──▶ HOME / back / scroll
 *
 * All drawing goes through camos/ui.h into the DPI panel frame buffers
 * (app_lcd_get_fb / app_lcd_flush, double-buffered full-screen flush).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "app_lcd.h"
#include "net/app_net.h"
#include "camos/ui.h"
#include "camos/ui_icons.h"
#include "camos/touch.h"
#include "browser/web_client.h"
#include "browser/html_text.h"
#include "browser/films.h"
#include "browser/browser.h"
#if CONFIG_EB_MEDIA_ENABLE
#include "media/media_player.h"
#include "media/pstats.h"
#include "media/sdcard.h"
#include <dirent.h>
#endif

static const char *TAG = "browser";

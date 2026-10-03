/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Измерение download-скорости через HTTP GET фиксированного объёма.
 * Использует публичный endpoint Cloudflare speed test (без API-ключа).
 *
 * Вердикт для фильма (профиль smooth ~250 kbps + запас):
 *   < 0.5 Мбит/с  — плохо, стрим будет рваться
 *   0.5…2 Мбит/с  — приемлемо для 320×180
 *   > 2 Мбит/с    — хорошо
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#include "net/app_net.h"
#include "net/app_eth.h"
#include "net/app_wifi.h"
#include "net/speed_test.h"

static const char *TAG = "speed_test";

#define SPEED_BYTES_TARGET   (512u * 1024u)
#define SPEED_TIMEOUT_MS     15000
#define SPEED_URL \
    "https://speed.cloudflare.com/__down?bytes=524288"

static speed_result_t s_last;
static speed_result_t s_hist[SPEED_HIST_N];
static size_t s_hist_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static speed_iface_t detect_iface(void)
{
#if CONFIG_EB_ETH_ENABLE
    if (app_eth_ready()) {
        return SPEED_IF_ETH;
    }
#endif
#if CONFIG_EB_WIFI_ENABLE
    if (app_wifi_ready()) {
        return SPEED_IF_WIFI;
    }
#endif
    if (app_net_ready()) {
        return SPEED_IF_UNKNOWN;
    }
    return SPEED_IF_UNKNOWN;
}

static const char *iface_name(speed_iface_t i)
{
    switch (i) {
    case SPEED_IF_ETH:  return "eth";
    case SPEED_IF_WIFI: return "wifi";
    default:            return "?";
    }
}

static void fill_note(speed_result_t *r)
{
    if (!r->ok) {
        strlcpy(r->note, "ошибка измерения", sizeof(r->note));
        return;
    }
    if (r->mbps < 0.5f) {
        strlcpy(r->note, "медленно — стрим будет рваться", sizeof(r->note));
    } else if (r->mbps < 2.0f) {
        strlcpy(r->note, "норм для smooth (320x180)", sizeof(r->note));
    } else {
        strlcpy(r->note, "хорошо для видео", sizeof(r->note));
    }
}

static void hist_push(const speed_result_t *r)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t i = SPEED_HIST_N - 1; i > 0; i--) {
        s_hist[i] = s_hist[i - 1];
    }
    s_hist[0] = *r;
    if (s_hist_count < SPEED_HIST_N) {
        s_hist_count++;
    }
    s_last = *r;
    portEXIT_CRITICAL(&s_lock);
}

typedef struct {
    uint32_t bytes;
    int64_t  t0_us;
} dl_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    dl_ctx_t *ctx = (dl_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        ctx->bytes += (uint32_t)evt->data_len;
    }
    return ESP_OK;
}

esp_err_t speed_test_run(speed_result_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->iface = detect_iface();
    strlcpy(out->iface_str, iface_name(out->iface), sizeof(out->iface_str));

    if (!app_net_ready()) {
        strlcpy(out->note, "нет сети", sizeof(out->note));
        hist_push(out);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "старт теста (%s), цель %u КБ...",
             out->iface_str, (unsigned)(SPEED_BYTES_TARGET / 1024));

    dl_ctx_t ctx = { .bytes = 0, .t0_us = esp_timer_get_time() };

    esp_http_client_config_t cfg = {
        .url = SPEED_URL,
        .event_handler = http_event,
        .user_data = &ctx,
        .timeout_ms = SPEED_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        strlcpy(out->note, "http init fail", sizeof(out->note));
        hist_push(out);
        return ESP_FAIL;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(cli);
    int64_t t1 = esp_timer_get_time();
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    uint32_t ms = (uint32_t)((t1 - t0) / 1000);
    if (ms == 0) {
        ms = 1;
    }

    out->bytes = ctx.bytes;
    out->ms = ms;
    out->mbps = (float)ctx.bytes * 8.0f / ((float)ms * 1000.0f);
    out->ok = (err == ESP_OK && status == 200 && ctx.bytes > 32 * 1024);
    fill_note(out);

    ESP_LOGI(TAG, "результат: %.2f Мбит/с, %u КБ за %u мс [%s] %s",
             out->mbps, (unsigned)(out->bytes / 1024), (unsigned)out->ms,
             out->iface_str, out->note);

    hist_push(out);
    return out->ok ? ESP_OK : ESP_FAIL;
}

const speed_result_t *speed_test_last(void)
{
    return &s_last;
}

size_t speed_test_history_count(void)
{
    return s_hist_count;
}

const speed_result_t *speed_test_history_get(size_t index)
{
    if (index >= s_hist_count) {
        return NULL;
    }
    return &s_hist[index];
}

bool speed_test_ok_for_smooth(const speed_result_t *r)
{
    return r != NULL && r->ok && r->mbps >= 0.5f;
}

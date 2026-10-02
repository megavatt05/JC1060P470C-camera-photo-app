/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser log server: the last ~20 KB of ESP_LOG output over HTTP.
 *
 * A tiny esp_log vprintf hook appends every formatted log line to a RAM
 * ring buffer (chained to the previous sink, so the UART keeps printing
 * as before). Once the board has an IP, an esp_http_server serves the
 * ring at:
 *
 *     http://<board-ip>/log        text/plain, oldest line first
 *
 * This gives terminal visibility without a USB cable: open the URL from
 * any PC/phone on the same LAN and read/copy the log (the exact data the
 * serial monitor would show).
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "net/app_eth.h"
#include "net/log_server.h"

static const char *TAG = "log_srv";

#define LOG_RING_SIZE   (20u * 1024)
#define LOG_LINE_MAX    512

static struct {
    portMUX_TYPE lock;
    uint8_t      buf[LOG_RING_SIZE];
    size_t       head;            /* write position            */
    size_t       filled;          /* bytes valid (<= SIZE)     */
    uint32_t     dropped;         /* lines lost to truncation  */
    vprintf_like_t prev_sink;
    bool         hooked;
} s_ring = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void ring_put(const char *s, size_t n)
{
    /* called with the spinlock held */
    for (size_t i = 0; i < n; i++) {
        s_ring.buf[s_ring.head] = (uint8_t)s[i];
        s_ring.head = (s_ring.head + 1) % LOG_RING_SIZE;
        if (s_ring.filled < LOG_RING_SIZE) {
            s_ring.filled++;
        } else {
            s_ring.dropped++;   /* one byte of the oldest line lost */
        }
    }
}

static int log_ring_vprintf(const char *format, va_list args)
{
    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), format, args);
    if (n > 0) {
        if (n > (int)sizeof(line) - 1) {
            n = (int)sizeof(line) - 1;
        }
        portENTER_CRITICAL(&s_ring.lock);
        ring_put(line, (size_t)n);
        portEXIT_CRITICAL(&s_ring.lock);
    }
    /* chain to the previous sink (UART console) */
    vprintf_like_t prev = s_ring.prev_sink;
    return (prev != NULL) ? prev(format, args) : n;
}

static esp_err_t log_http_handler(httpd_req_t *req)
{
    static char snap[LOG_RING_SIZE + 64];

    portENTER_CRITICAL(&s_ring.lock);
    size_t n = s_ring.filled;
    if (n > 0) {
        size_t tail = (s_ring.head + LOG_RING_SIZE - n) % LOG_RING_SIZE;
        size_t first = LOG_RING_SIZE - tail;
        if (first > n) {
            first = n;
        }
        memcpy(snap, s_ring.buf + tail, first);
        if (n > first) {
            memcpy(snap + first, s_ring.buf, n - first);
        }
    }
    uint32_t dropped = s_ring.dropped;
    portEXIT_CRITICAL(&s_ring.lock);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    size_t total = 0;
    if (dropped > 0) {
        total = (size_t)snprintf(snap + n, sizeof(snap) - n,
                "[... старые строки вытеснены: %lu байт ...]\r\n",
                (unsigned long)dropped);
    }
    esp_err_t err = httpd_resp_send(req, snap, n + total);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t root_http_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char msg[128];
    snprintf(msg, sizeof(msg),
             "CamBrowser log: http://%s/log\r\n", app_eth_ip_str());
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t log_server_start(void)
{
    static bool started;

    if (!s_ring.hooked) {
        s_ring.prev_sink = esp_log_set_vprintf(log_ring_vprintf);
        s_ring.hooked = true;
        ESP_LOGI(TAG, "log ring hooked (%u KB)", LOG_RING_SIZE / 1024);
    }
    if (started) {
        return ESP_OK;
    }

    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    httpd_uri_t uri_log = { .uri = "/log", .method = HTTP_GET,
                            .handler = log_http_handler, .user_ctx = NULL };
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET,
                             .handler = root_http_handler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &uri_log);
    httpd_register_uri_handler(server, &uri_root);

    started = true;
    ESP_LOGI(TAG, "log server ready: http://%s/log", app_eth_ip_str());
    return ESP_OK;
}

/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser log server: кольцевой буфер ESP_LOG + страница теста скорости.
 *
 *   http://<board-ip>/log     — текст логов (plain)
 *   http://<board-ip>/speed   — HTML: последний тест + история
 *   http://<board-ip>/        — подсказка
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "net/app_net.h"
#include "net/log_server.h"
#include "net/speed_test.h"

static const char *TAG = "log_srv";

#define LOG_RING_SIZE   (20u * 1024)
#define LOG_LINE_MAX    512

static struct {
    portMUX_TYPE lock;
    uint8_t      buf[LOG_RING_SIZE];
    size_t       head;
    size_t       filled;
    uint32_t     dropped;
    vprintf_like_t prev_sink;
    bool         hooked;
} s_ring = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static void ring_put(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_ring.buf[s_ring.head] = (uint8_t)s[i];
        s_ring.head = (s_ring.head + 1) % LOG_RING_SIZE;
        if (s_ring.filled < LOG_RING_SIZE) {
            s_ring.filled++;
        } else {
            s_ring.dropped++;
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
    return httpd_resp_send(req, snap, n + total);
}

static esp_err_t speed_http_handler(httpd_req_t *req)
{
    static char page[4096];
    const speed_result_t *last = speed_test_last();
    size_t n = 0;

    n += (size_t)snprintf(page + n, sizeof(page) - n,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>CamBrowser speed</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:16px}"
        "h1{font-size:1.2rem;color:#6cf}"
        ".card{background:#1a1a1a;border:1px solid #333;border-radius:8px;"
        "padding:12px;margin:8px 0}"
        ".ok{color:#6f6}.bad{color:#f66}.mid{color:#fc6}"
        "table{width:100%%;border-collapse:collapse;font-size:.9rem}"
        "td,th{border-bottom:1px solid #333;padding:6px 4px;text-align:left}"
        "a{color:#6cf}"
        "</style></head><body>"
        "<h1>Тест скорости CamBrowser</h1>");

    if (last && last->ms > 0) {
        const char *cls = (!last->ok || last->mbps < 0.5f) ? "bad"
                        : (last->mbps < 2.0f) ? "mid" : "ok";
        n += (size_t)snprintf(page + n, sizeof(page) - n,
            "<div class=card>"
            "<div>Последний тест: <b class=%s>%.2f Мбит/с</b></div>"
            "<div>Интерфейс: <b>%s</b> · %u КБ за %u мс</div>"
            "<div class=%s>%s</div>"
            "</div>",
            cls, (double)last->mbps,
            last->iface_str,
            (unsigned)(last->bytes / 1024), (unsigned)last->ms,
            cls, last->note);
    } else {
        n += (size_t)snprintf(page + n, sizeof(page) - n,
            "<div class=card>Ещё не запускали. Тест идёт автоматически "
            "перед фильмом.</div>");
    }

    size_t hc = speed_test_history_count();
    if (hc > 0) {
        n += (size_t)snprintf(page + n, sizeof(page) - n,
            "<div class=card><b>История (в RAM, до %d)</b>"
            "<table><tr><th>#</th><th>Мбит/с</th><th>if</th><th>КБ</th>"
            "<th>мс</th><th>заметка</th></tr>", SPEED_HIST_N);
        for (size_t i = 0; i < hc && n + 120 < sizeof(page); i++) {
            const speed_result_t *h = speed_test_history_get(i);
            if (!h) break;
            n += (size_t)snprintf(page + n, sizeof(page) - n,
                "<tr><td>%u</td><td>%.2f</td><td>%s</td><td>%u</td>"
                "<td>%u</td><td>%s</td></tr>",
                (unsigned)i, (double)h->mbps, h->iface_str,
                (unsigned)(h->bytes / 1024), (unsigned)h->ms, h->note);
        }
        n += (size_t)snprintf(page + n, sizeof(page) - n, "</table></div>");
    }

    n += (size_t)snprintf(page + n, sizeof(page) - n,
        "<p><a href=/log>логи</a> · IP %s</p>"
        "<p style=color:#888;font-size:.8rem>"
        "Порог для smooth-видео: ≥ 0.5 Мбит/с. Рекомендуется ≥ 2 Мбит/с."
        "</p></body></html>",
        app_net_ip_str()[0] ? app_net_ip_str() : "—");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, page, n);
}

static esp_err_t root_http_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char msg[192];
    snprintf(msg, sizeof(msg),
             "CamBrowser\r\n"
             "  log:   http://%s/log\r\n"
             "  speed: http://%s/speed\r\n",
             app_net_ip_str(), app_net_ip_str());
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static httpd_handle_t s_httpd;

esp_err_t log_server_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        ESP_LOGI(TAG, "httpd остановлен (порт 80 свободен для портала)");
    }
    return ESP_OK;
}

esp_err_t log_server_start(void)
{
    if (!s_ring.hooked) {
        s_ring.prev_sink = esp_log_set_vprintf(log_ring_vprintf);
        s_ring.hooked = true;
        ESP_LOGI(TAG, "кольцо логов (%u КБ)", LOG_RING_SIZE / 1024);
    }
    if (s_httpd) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    httpd_uri_t uri_log = { .uri = "/log", .method = HTTP_GET,
                            .handler = log_http_handler, .user_ctx = NULL };
    httpd_uri_t uri_speed = { .uri = "/speed", .method = HTTP_GET,
                              .handler = speed_http_handler, .user_ctx = NULL };
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET,
                             .handler = root_http_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_httpd, &uri_log);
    httpd_register_uri_handler(s_httpd, &uri_speed);
    httpd_register_uri_handler(s_httpd, &uri_root);
    ESP_LOGI(TAG, "log: http://%s/log  speed: http://%s/speed",
             app_net_ip_str(), app_net_ip_str());
    return ESP_OK;
}

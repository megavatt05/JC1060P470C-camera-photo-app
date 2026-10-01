/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser web client: HTTPS GET via esp_http_client + mbedTLS with the
 * ESP x509 certificate bundle (verification always on).
 *
 * Design notes:
 *  - The response body lands in a PSRAM buffer capped at
 *    CONFIG_EB_MAX_BODY_KB (SERP pages are 100-300 KB, pages larger than
 *    the cap are truncated, which is fine for a text extractor).
 *  - "Accept-Encoding: identity" is set explicitly: esp_http_client does
 *    not decompress gzip, so we politely ask servers for the raw body.
 *  - A desktop Chrome User-Agent keeps Google/DDG from serving their
 *    most degraded markup.
 *  - A CONSENT cookie is sent to soften the EU Google consent redirect.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#include "browser/web_client.h"

static const char *TAG = "browser_http";

#define WEB_CLIENT_UA \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

static const char *GOOGLE_CONSENT_COOKIE =
    "CONSENT=YES+cb.20210720-08-p0.en+FX+700";

esp_err_t web_get(const char *url, const char *extra_cookie,
                  char **out_body, size_t *out_len, int *out_status)
{
    if (url == NULL || out_body == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    *out_len = 0;
    if (out_status) {
        *out_status = 0;
    }

    const size_t cap = (size_t)CONFIG_EB_MAX_BODY_KB * 1024;
    char *buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for %u KB body", (unsigned)(cap / 1024));
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .timeout_ms = 15000,
        .max_redirects = 10,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .user_agent = WEB_CLIENT_UA,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Accept-Language", "en-US,en;q=0.9");
    if (extra_cookie != NULL) {
        esp_http_client_set_header(client, "Cookie", extra_cookie);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        return err;
    }

    int status = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "GET %s -> %d", url, status);

    size_t total = 0;
    while (total < cap) {
        int n = esp_http_client_read(client, buf + total, (int)(cap - total));
        if (n < 0) {
            ESP_LOGW(TAG, "read error mid-body");
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;  /* EOF or chunk boundary done */
        }
        total += (size_t)n;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (out_status) {
        *out_status = status;
    }

    if (err != ESP_OK && total == 0) {
        free(buf);
        return err;
    }

    *out_body = buf;
    *out_len = total;
    return ESP_OK;
}

/* --- URL escaping for the query string ---------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool web_url_decode(const char *src, char *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0) {
        return false;
    }

    size_t o = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        }
        if (c == '%') {
            int h = hexval(src[i + 1]);
            int l = (h >= 0) ? hexval(src[i + 2]) : -1;
            if (h < 0 || l < 0) {
                return false;   /* malformed escape */
            }
            c = (char)((h << 4) | l);
            i += 2;
        }
        if (o + 1 >= dst_size) {
            return false;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return true;
}

size_t web_url_encode(const char *src, char *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0) {
        return 0;
    }

    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (const char *p = src; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';

        if (safe) {
            if (o + 1 >= dst_size) break;
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= dst_size) break;
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
    return o;
}

esp_err_t web_search(const char *query, bool use_google,
                     char **out_body, size_t *out_len)
{
    char q_enc[512];
    web_url_encode(query, q_enc, sizeof(q_enc));

    char url[640];
    if (use_google) {
        snprintf(url, sizeof(url),
                 "https://www.google.com/search?q=%s&num=20&hl=en",
                 q_enc);
        return web_get(url, GOOGLE_CONSENT_COOKIE, out_body, out_len, NULL);
    }

    snprintf(url, sizeof(url),
             "https://html.duckduckgo.com/html/?q=%s",
             q_enc);
    return web_get(url, NULL, out_body, out_len, NULL);
}

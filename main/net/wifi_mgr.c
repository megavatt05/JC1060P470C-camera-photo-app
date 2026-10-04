/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi‑Fi менеджер — написан с нуля.
 *
 * Порядок init (как station example + esp_hosted):
 *   nvs → netif → event_loop → netif STA → esp_wifi_init → handlers
 *   → set_mode/config → start → (STA_START → connect)
 *
 * SoftAP портал: WIFI_MODE_APSTA, HTTP :8080 (не конфликтует с /log на :80).
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "net/speed_test.h"
#include "net/log_server.h"
#include "net/wifi_mgr.h"

static const char *TAG = "wifi_mgr";

#define NVS_NS              "wificfg"
#define NVS_SSID            "ssid"
#define NVS_PASS            "pass"
#define AP_SSID             "CamBrowser-Setup"
#define AP_CHANNEL          6
#define HTTP_PORT           80
#define MAX_RETRY           7
#define CONNECT_TIMEOUT_MS  20000

#define BIT_OK   BIT0
#define BIT_FAIL BIT1

typedef struct {
    bool                 ready;
    bool                 do_connect; /* true → на STA_START вызвать connect */
    wifi_mgr_state_t     state;
    int                  retry;
    char                 status[40];
    char                 ip[16];
    char                 ssid[WIFI_MGR_SSID_MAX + 1];
    char                 pass[WIFI_MGR_PASS_MAX + 1];
    wifi_mgr_ap_t        scan[WIFI_MGR_SCAN_MAX];
    int                  scan_n;
    EventGroupHandle_t   events;
    esp_netif_t         *netif_sta;
    esp_netif_t         *netif_ap;
    httpd_handle_t       httpd;
    char                 portal_ssid[WIFI_MGR_SSID_MAX + 1];
} mgr_t;

static mgr_t M;

static void status_set(const char *s)
{
    strlcpy(M.status, s, sizeof(M.status));
    ESP_LOGI(TAG, "%s", s);
}

/* ---- NVS ---------------------------------------------------------------- */

static void creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(M.ssid);
    if (nvs_get_str(h, NVS_SSID, M.ssid, &n) != ESP_OK) {
        M.ssid[0] = '\0';
    }
    n = sizeof(M.pass);
    if (nvs_get_str(h, NVS_PASS, M.pass, &n) != ESP_OK) {
        M.pass[0] = '\0';
    }
    nvs_close(h);
}

static void creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, NVS_SSID, ssid ? ssid : "");
    nvs_set_str(h, NVS_PASS, pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
    strlcpy(M.ssid, ssid ? ssid : "", sizeof(M.ssid));
    strlcpy(M.pass, pass ? pass : "", sizeof(M.pass));
    ESP_LOGI(TAG, "сохранено SSID=%s", M.ssid);
}

/* ---- events (station example) ------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (M.do_connect) {
            esp_wifi_connect();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        M.ip[0] = '\0';
        int reason = 0;
        if (data) {
            const wifi_event_sta_disconnected_t *d = data;
            reason = d->reason;
            ESP_LOGW(TAG, "обрыв reason=%d", reason);
        }
        if (!M.do_connect) {
            return;
        }
        if (M.retry < MAX_RETRY) {
            M.retry++;
            char b[32];
            snprintf(b, sizeof(b), "повтор %d/%d", M.retry, MAX_RETRY);
            status_set(b);
            M.state = WIFI_MGR_CONNECTING;
            esp_wifi_connect();
        } else {
            M.state = WIFI_MGR_FAIL;
            status_set(reason == 201 ? "сеть не найдена" :
                       (reason == 202 || reason == 15) ? "неверный пароль?" :
                       reason == 203 ? "assoc fail" : "нет связи");
            xEventGroupSetBits(M.events, BIT_FAIL);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(M.ip, sizeof(M.ip), IPSTR, IP2STR(&e->ip_info.ip));
        M.retry = 0;
        M.state = WIFI_MGR_CONNECTED;
        status_set("есть IP");
        ESP_LOGI(TAG, "IP %s", M.ip);
        if (M.netif_sta) {
            esp_netif_set_default_netif(M.netif_sta);
        }
        xEventGroupSetBits(M.events, BIT_OK);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        status_set("откройте http://192.168.4.1");
    }
}

static esp_err_t stack_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }

    if (!M.events) {
        M.events = xEventGroupCreate();
    }
    if (!M.netif_sta) {
        M.netif_sta = esp_netif_create_default_wifi_sta();
    }

    static bool wifi_ok;
    if (!wifi_ok) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, NULL, NULL));
        wifi_ok = true;
    }
    return ESP_OK;
}

/* ---- API ---------------------------------------------------------------- */

esp_err_t wifi_mgr_init(void)
{
#if !CONFIG_EB_WIFI_ENABLE
    status_set("выключен");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (M.ready) {
        return ESP_OK;
    }
    memset(&M, 0, sizeof(M));
    status_set("старт...");
    if (stack_init() != ESP_OK) {
        status_set("ошибка стека");
        return ESP_FAIL;
    }
    creds_load();
    M.ready = true;

    if (M.ssid[0]) {
        return wifi_mgr_connect(M.ssid, M.pass);
    }
    if (strcmp(CONFIG_EB_WIFI_SSID, "myssid") != 0 && CONFIG_EB_WIFI_SSID[0]) {
        return wifi_mgr_connect(CONFIG_EB_WIFI_SSID, CONFIG_EB_WIFI_PASSWORD);
    }

    /* Нет кредов: STA без connect */
    M.do_connect = false;
    wifi_config_t empty = { 0 };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &empty));
    ESP_ERROR_CHECK(esp_wifi_start());
    M.state = WIFI_MGR_IDLE;
    status_set("нет сети — откройте Wi-Fi");
    return ESP_OK;
#endif
}

wifi_mgr_state_t wifi_mgr_state(void) { return M.state; }
const char *wifi_mgr_status_str(void) { return M.status[0] ? M.status : "—"; }
const char *wifi_mgr_ip_str(void) { return M.ip; }
const char *wifi_mgr_saved_ssid(void) { return M.ssid; }
bool wifi_mgr_ready(void) { return M.state == WIFI_MGR_CONNECTED && M.ip[0]; }

esp_err_t wifi_mgr_scan(void)
{
#if !CONFIG_EB_WIFI_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_ERROR_CHECK(stack_init());
    M.state = WIFI_MGR_SCANNING;
    M.scan_n = 0;
    status_set("скан...");

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        M.do_connect = false;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    wifi_scan_config_t sc = { 0 };
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        status_set("скан ошибка");
        M.state = WIFI_MGR_FAIL;
        return ESP_FAIL;
    }

    uint16_t n = WIFI_MGR_SCAN_MAX;
    wifi_ap_record_t rec[WIFI_MGR_SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) {
        n = 0;
    }
    M.scan_n = (int)n;
    for (int i = 0; i < M.scan_n; i++) {
        strlcpy(M.scan[i].ssid, (const char *)rec[i].ssid, sizeof(M.scan[i].ssid));
        M.scan[i].rssi = rec[i].rssi;
        M.scan[i].authmode = (uint8_t)rec[i].authmode;
        M.scan[i].is_open = (rec[i].authmode == WIFI_AUTH_OPEN);
    }
    char b[24];
    snprintf(b, sizeof(b), "найдено %d", M.scan_n);
    status_set(b);
    M.state = WIFI_MGR_IDLE;
    return ESP_OK;
#endif
}

int wifi_mgr_scan_count(void) { return M.scan_n; }

const wifi_mgr_ap_t *wifi_mgr_scan_get(int i)
{
    return (i >= 0 && i < M.scan_n) ? &M.scan[i] : NULL;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
#if !CONFIG_EB_WIFI_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_ERROR_CHECK(stack_init());

    const char *pw = password;
    if (!pw && M.ssid[0] && strcmp(ssid, M.ssid) == 0) {
        pw = M.pass;
    }

    if (M.httpd) {
        httpd_stop(M.httpd);
        M.httpd = NULL;
    }

    M.do_connect = true;
    M.retry = 0;
    M.ip[0] = '\0';
    M.state = WIFI_MGR_CONNECTING;
    status_set("подключение...");
    xEventGroupClearBits(M.events, BIT_OK | BIT_FAIL);

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (pw) {
        strlcpy((char *)cfg.sta.password, pw, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = (pw && pw[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        status_set("start fail");
        return e;
    }
    esp_wifi_connect(); /* если STA уже был START */

    EventBits_t bits = xEventGroupWaitBits(
        M.events, BIT_OK | BIT_FAIL, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

    if (bits & BIT_OK) {
        creds_save(ssid, pw ? pw : "");
        return ESP_OK;
    }
    M.state = WIFI_MGR_FAIL;
    if (!(bits & BIT_FAIL)) {
        status_set("таймаут");
    }
    return ESP_ERR_TIMEOUT;
#endif
}

/* ---- SoftAP + HTTP ------------------------------------------------------ */



/* HTML страница настройки (порт 80 → http://192.168.4.1/) */
static esp_err_t http_send_portal(httpd_req_t *req)
{
    char page[1400];
    const char *pre = M.portal_ssid[0] ? M.portal_ssid : "";
    /* без вложенных " внутри C-строки — charset=utf-8 без кавычек в HTML */
    snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>CamBrowser Wi-Fi</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;"
        "margin:0;padding:24px}"
        "h1{color:#58a6ff}"
        "input,button{display:block;width:100%%;max-width:360px;box-sizing:border-box;"
        "padding:12px;margin:8px 0;font-size:16px;border-radius:8px}"
        "input{border:1px solid #30363d;background:#161b22;color:#e6edf3}"
        "button{background:#238636;color:#fff;border:0}"
        "</style></head><body>"
        "<h1>Настройка Wi-Fi</h1>"
        "<p>Только <b>2.4 ГГц</b>. Введите пароль сети.</p>"
        "<form method=POST action=/save>"
        "<label>SSID</label>"
        "<input name=ssid required maxlength=32 value=\"%s\">"
        "<label>Пароль</label>"
        "<input name=pass type=password maxlength=64 autofocus>"
        "<button type=submit>Сохранить и подключить</button>"
        "</form></body></html>",
        pre);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_root(httpd_req_t *req)
{
    return http_send_portal(req);
}

/* Captive portal probes (Android / iOS / Windows) — редирект на форму */
static esp_err_t http_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    out[0] = '\0';
    char k[16];
    snprintf(k, sizeof(k), "%s=", key);
    const char *p = strstr(body, k);
    if (!p) {
        return;
    }
    p += strlen(k);
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < out_sz) {
        char c = *p++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && p[0] && p[1]) {
            char hex[3] = { p[0], p[1], 0 };
            c = (char)strtol(hex, NULL, 16);
            p += 2;
        }
        out[i++] = c;
    }
    out[i] = '\0';
}

static esp_err_t http_post_save(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    }
    body[n] = '\0';

    char ssid[WIFI_MGR_SSID_MAX + 1];
    char pass[WIFI_MGR_PASS_MAX + 1];
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>OK</title></head>"
        "<body style=\"background:#0d1117;color:#e6edf3;font-family:sans-serif;padding:24px\">"
        "<h1>Сохранено</h1><p>Подключаюсь к сети… Можно закрыть эту страницу.</p>"
        "</body></html>");

    if (ssid[0]) {
        wifi_mgr_stop_portal();
        wifi_mgr_connect(ssid, pass);
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_start_portal_ssid(const char *ssid)
{
    if (ssid && ssid[0]) {
        strlcpy(M.portal_ssid, ssid, sizeof(M.portal_ssid));
    } else {
        M.portal_ssid[0] = '\0';
    }
    return wifi_mgr_start_portal();
}

esp_err_t wifi_mgr_start_portal(void)
{
#if !CONFIG_EB_WIFI_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_ERROR_CHECK(stack_init());
    M.do_connect = false;

    /* порт 80: останавливаем log_server, иначе httpd_start fail */
    log_server_stop();

    if (!M.netif_ap) {
        M.netif_ap = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }

    if (!M.httpd) {
        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.server_port = HTTP_PORT; /* 80 → http://192.168.4.1/ без порта */
        hc.max_uri_handlers = 12;
        hc.lru_purge_enable = true;
        if (httpd_start(&M.httpd, &hc) == ESP_OK) {
            const httpd_uri_t uris[] = {
                { .uri = "/", .method = HTTP_GET, .handler = http_get_root },
                { .uri = "/save", .method = HTTP_POST, .handler = http_post_save },
                { .uri = "/generate_204", .method = HTTP_GET, .handler = http_captive },
                { .uri = "/gen_204", .method = HTTP_GET, .handler = http_captive },
                { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = http_captive },
                { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = http_captive },
                { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = http_captive },
                { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = http_captive },
                { .uri = "/favicon.ico", .method = HTTP_GET, .handler = http_favicon },
            };
            for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
                httpd_register_uri_handler(M.httpd, &uris[i]);
            }
            ESP_LOGI(TAG, "HTTP портал на порту %d", HTTP_PORT);
        } else {
            ESP_LOGE(TAG, "httpd_start на :80 не удался");
            status_set("ошибка HTTP :80");
            return ESP_FAIL;
        }
    }
    M.state = WIFI_MGR_AP_MODE;
    status_set("http://192.168.4.1");
    return ESP_OK;
#endif
}

esp_err_t wifi_mgr_stop_portal(void)
{
    if (M.httpd) {
        httpd_stop(M.httpd);
        M.httpd = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (M.state == WIFI_MGR_AP_MODE) {
        M.state = WIFI_MGR_IDLE;
        status_set("портал выкл");
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_test_internet(wifi_mgr_test_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    M.state = WIFI_MGR_TESTING;
    status_set("тест HTTP...");

    int64_t t0 = esp_timer_get_time();
    esp_http_client_config_t cfg = {
        .url = "http://connectivitycheck.gstatic.com/generate_204",
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        strlcpy(out->note, "init fail", sizeof(out->note));
        M.state = WIFI_MGR_FAIL;
        return ESP_FAIL;
    }
    esp_err_t e = esp_http_client_perform(c);
    out->http_status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    out->latency_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (e != ESP_OK || (out->http_status != 204 && out->http_status != 200)) {
        snprintf(out->note, sizeof(out->note), "нет сети HTTP %d", out->http_status);
        status_set(out->note);
        out->ok = false;
        M.state = WIFI_MGR_FAIL;
        return ESP_FAIL;
    }

    status_set("тест скорости...");
    speed_result_t sp;
    if (speed_test_run(&sp) == ESP_OK) {
        out->mbps = sp.mbps;
        snprintf(out->note, sizeof(out->note), "OK %.1f Мбит/с %uмс",
                 (double)sp.mbps, (unsigned)out->latency_ms);
    } else {
        snprintf(out->note, sizeof(out->note), "HTTP OK %uмс", (unsigned)out->latency_ms);
    }
    out->ok = true;
    status_set(out->note);
    M.state = WIFI_MGR_CONNECTED;
    return ESP_OK;
}

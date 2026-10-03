/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi‑Fi менеджер: NVS, скан, STA, SoftAP-портал, тест интернета.
 * Радио — на ESP32-C6 через esp_hosted / esp_wifi_remote.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "net/wifi_mgr.h"
#include "net/speed_test.h"

static const char *TAG = "wifi_mgr";

#define NVS_NS          "wifi_mgr"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define PORTAL_SSID     "CamBrowser-Setup"
#define PORTAL_PASS     ""          /* открытая сеть для настройки */
#define CONNECT_BITS    BIT0
#define FAIL_BITS       BIT1

static struct {
    bool inited;
    volatile wifi_mgr_state_t state;
    char status[48];
    char ip[16];
    char saved_ssid[WIFI_MGR_SSID_MAX + 1];
    char saved_pass[WIFI_MGR_PASS_MAX + 1];
    wifi_mgr_ap_t aps[WIFI_MGR_SCAN_MAX];
    int ap_count;
    EventGroupHandle_t eg;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    httpd_handle_t portal_httpd;
    int retry;
} s;

static void set_status(const char *sstr)
{
    strlcpy(s.status, sstr, sizeof(s.status));
    ESP_LOGI(TAG, "%s", sstr);
}

static esp_err_t nvs_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t sz = sizeof(s.saved_ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, s.saved_ssid, &sz) != ESP_OK) {
        s.saved_ssid[0] = '\0';
    }
    sz = sizeof(s.saved_pass);
    if (nvs_get_str(h, NVS_KEY_PASS, s.saved_pass, &sz) != ESP_OK) {
        s.saved_pass[0] = '\0';
    }
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, NVS_KEY_SSID, ssid ? ssid : "");
    nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        strlcpy(s.saved_ssid, ssid ? ssid : "", sizeof(s.saved_ssid));
        strlcpy(s.saved_pass, pass ? pass : "", sizeof(s.saved_pass));
        ESP_LOGI(TAG, "NVS: сохранено SSID=\"%s\"", s.saved_ssid);
    }
    return err;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_STA_START) {
        if (s.state != WIFI_MGR_AP_MODE) {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s.ip[0] = '\0';
        if (s.state == WIFI_MGR_CONNECTING && s.retry < 10) {
            s.retry++;
            set_status("wifi: переподключение...");
            esp_wifi_connect();
        } else if (s.state == WIFI_MGR_CONNECTING) {
            s.state = WIFI_MGR_FAIL;
            set_status("wifi: нет связи");
            if (s.eg) {
                xEventGroupSetBits(s.eg, FAIL_BITS);
            }
        } else if (s.state == WIFI_MGR_CONNECTED) {
            s.state = WIFI_MGR_CONNECTING;
            set_status("wifi: обрыв, reconnect");
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_AP_START) {
        set_status("портал: 192.168.4.1:8080");
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&e->ip_info.ip));
        s.retry = 0;
        s.state = WIFI_MGR_CONNECTED;
        set_status("wifi: есть IP");
        ESP_LOGI(TAG, "STA IP: %s", s.ip);
        if (s.eg) {
            xEventGroupSetBits(s.eg, CONNECT_BITS);
        }
    }
}

static esp_err_t ensure_stack(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (s.sta_netif == NULL) {
        s.sta_netif = esp_netif_create_default_wifi_sta();
    }
    static bool wifi_inited;
    if (!wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
            return err;
        }
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip, NULL));
        wifi_inited = true;
    }
    if (s.eg == NULL) {
        s.eg = xEventGroupCreate();
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_init(void)
{
#if !CONFIG_EB_WIFI_ENABLE
    set_status("wifi: выключен в Kconfig");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s.inited) {
        return ESP_OK;
    }
    memset(&s, 0, sizeof(s));
    set_status("wifi: инициализация");
    esp_err_t err = ensure_stack();
    if (err != ESP_OK) {
        set_status("wifi: сбой стека");
        return err;
    }
    nvs_load();
    s.inited = true;

    /* Если в NVS есть SSID — подключаемся; иначе просто STA idle */
    if (s.saved_ssid[0]) {
        return wifi_mgr_connect(s.saved_ssid, s.saved_pass);
    }
    /* Запасной вариант: Kconfig SSID */
    if (strlen(CONFIG_EB_WIFI_SSID) > 0 &&
        strcmp(CONFIG_EB_WIFI_SSID, "myssid") != 0) {
        return wifi_mgr_connect(CONFIG_EB_WIFI_SSID, CONFIG_EB_WIFI_PASSWORD);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s.state = WIFI_MGR_IDLE;
    set_status("wifi: нет сохранённой сети");
    return ESP_OK;
#endif
}

wifi_mgr_state_t wifi_mgr_state(void) { return s.state; }
const char *wifi_mgr_status_str(void) { return s.status[0] ? s.status : "wifi: —"; }
const char *wifi_mgr_ip_str(void) { return s.ip; }
bool wifi_mgr_ready(void) { return s.state == WIFI_MGR_CONNECTED && s.ip[0]; }
const char *wifi_mgr_saved_ssid(void) { return s.saved_ssid; }

esp_err_t wifi_mgr_scan(void)
{
#if !CONFIG_EB_WIFI_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t err = ensure_stack();
    if (err != ESP_OK) {
        return err;
    }
    s.state = WIFI_MGR_SCANNING;
    s.ap_count = 0;
    set_status("wifi: сканирование...");

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        set_status("wifi: скан не удался");
        s.state = WIFI_MGR_FAIL;
        return err;
    }
    uint16_t n = WIFI_MGR_SCAN_MAX;
    wifi_ap_record_t rec[WIFI_MGR_SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) {
        n = 0;
    }
    s.ap_count = (int)n;
    for (int i = 0; i < s.ap_count; i++) {
        strlcpy(s.aps[i].ssid, (const char *)rec[i].ssid, sizeof(s.aps[i].ssid));
        s.aps[i].rssi = rec[i].rssi;
        s.aps[i].authmode = (uint8_t)rec[i].authmode;
        s.aps[i].is_open = (rec[i].authmode == WIFI_AUTH_OPEN);
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "wifi: найдено %d", s.ap_count);
    set_status(buf);
    s.state = WIFI_MGR_IDLE;
    return ESP_OK;
#endif
}

int wifi_mgr_scan_count(void) { return s.ap_count; }

const wifi_mgr_ap_t *wifi_mgr_scan_get(int index)
{
    if (index < 0 || index >= s.ap_count) {
        return NULL;
    }
    return &s.aps[index];
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
#if !CONFIG_EB_WIFI_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_stack();
    if (err != ESP_OK) {
        return err;
    }
    s.state = WIFI_MGR_CONNECTING;
    s.retry = 0;
    s.ip[0] = '\0';
    set_status("wifi: подключение...");
    if (s.eg) {
        xEventGroupClearBits(s.eg, CONNECT_BITS | FAIL_BITS);
    }

    wifi_config_t wcfg = { 0 };
    strlcpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid));
    /* NULL password → берём из NVS (кнопка «ПОДКЛ NVS») */
    const char *pass = password;
    if (pass == NULL && s.saved_ssid[0] && strcmp(ssid, s.saved_ssid) == 0) {
        pass = s.saved_pass;
    }
    if (pass) {
        strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    }
    wcfg.sta.threshold.authmode = pass && pass[0]
                                      ? WIFI_AUTH_WPA2_PSK
                                      : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    /* Ждём до 15 с */
    EventBits_t bits = xEventGroupWaitBits(s.eg, CONNECT_BITS | FAIL_BITS,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & CONNECT_BITS) {
        nvs_save(ssid, password ? password : "");
        return ESP_OK;
    }
    s.state = WIFI_MGR_FAIL;
    set_status("wifi: таймаут");
    return ESP_ERR_TIMEOUT;
#endif
}

/* ---------- SoftAP портал ---------- */

static esp_err_t portal_root_get(httpd_req_t *req)
{
    static const char *html =
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>CamBrowser Wi‑Fi</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:16px}"
        "h1{color:#6cf;font-size:1.3rem}"
        "label{display:block;margin:12px 0 4px}"
        "input{width:100%;max-width:320px;padding:10px;border-radius:6px;border:1px solid #444;background:#1a1a1a;color:#eee}"
        "button{margin-top:16px;padding:12px 24px;background:#06c;color:#fff;border:0;border-radius:8px;font-size:1rem}"
        ".hint{color:#888;font-size:.85rem;margin-top:16px}"
        "</style></head><body>"
        "<h1>Настройка Wi‑Fi</h1>"
        "<p>Плата: CamBrowser (ESP32‑P4 + C6). Только <b>2.4 ГГц</b>.</p>"
        "<form method=POST action=/save>"
        "<label>Имя сети (SSID)</label>"
        "<input name=ssid required maxlength=32 placeholder=\"Домашний Wi‑Fi\">"
        "<label>Пароль</label>"
        "<input name=pass type=password maxlength=64 placeholder=\"WPA2 пароль\">"
        "<button type=submit>Сохранить и подключить</button>"
        "</form>"
        "<p class=hint>После сохранения плата переподключится к вашей сети. "
        "Статус смотрите на экране и в serial.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_save_post(httpd_req_t *req)
{
    char buf[192];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    }
    buf[n] = '\0';
    /* application/x-www-form-urlencoded: ssid=...&pass=... */
    char ssid[WIFI_MGR_SSID_MAX + 1] = { 0 };
    char pass[WIFI_MGR_PASS_MAX + 1] = { 0 };
    char *p = strstr(buf, "ssid=");
    if (p) {
        p += 5;
        char *e = strchr(p, '&');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > WIFI_MGR_SSID_MAX) {
            len = WIFI_MGR_SSID_MAX;
        }
        memcpy(ssid, p, len);
        ssid[len] = '\0';
        /* минимальный urldecode пробела */
        for (char *q = ssid; *q; q++) {
            if (*q == '+') {
                *q = ' ';
            }
        }
    }
    p = strstr(buf, "pass=");
    if (p) {
        p += 5;
        char *e = strchr(p, '&');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > WIFI_MGR_PASS_MAX) {
            len = WIFI_MGR_PASS_MAX;
        }
        memcpy(pass, p, len);
        pass[len] = '\0';
        for (char *q = pass; *q; q++) {
            if (*q == '+') {
                *q = ' ';
            }
        }
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body style='background:#111;color:#eee;font-family:sans-serif'>"
        "<h2>Сохранено</h2><p>Подключаюсь к сети... Смотрите экран платы.</p>"
        "</body></html>");

    /* Отложенное переподключение — ответ HTTP уже ушёл */
    if (ssid[0]) {
        nvs_save(ssid, pass);
        /* остановить AP чуть позже в той же задаче нельзя легко — connect сам сменит режим */
        wifi_mgr_stop_portal();
        wifi_mgr_connect(ssid, pass);
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_start_portal(void)
{
#if !CONFIG_EB_WIFI_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t err = ensure_stack();
    if (err != ESP_OK) {
        return err;
    }
    if (s.ap_netif == NULL) {
        s.ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, PORTAL_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(PORTAL_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s.portal_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 8080;
        cfg.max_uri_handlers = 6;
        if (httpd_start(&s.portal_httpd, &cfg) == ESP_OK) {
            httpd_uri_t u1 = { .uri = "/", .method = HTTP_GET, .handler = portal_root_get };
            httpd_uri_t u2 = { .uri = "/save", .method = HTTP_POST, .handler = portal_save_post };
            httpd_register_uri_handler(s.portal_httpd, &u1);
            httpd_register_uri_handler(s.portal_httpd, &u2);
        }
    }
    s.state = WIFI_MGR_AP_MODE;
    set_status("AP CamBrowser-Setup :8080");
    return ESP_OK;
#endif
}

esp_err_t wifi_mgr_stop_portal(void)
{
    if (s.portal_httpd) {
        httpd_stop(s.portal_httpd);
        s.portal_httpd = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (s.state == WIFI_MGR_AP_MODE) {
        s.state = WIFI_MGR_IDLE;
        set_status("портал выключен");
    }
    return ESP_OK;
}

/* ---------- Тест интернета ---------- */

esp_err_t wifi_mgr_test_internet(wifi_mgr_test_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    s.state = WIFI_MGR_TESTING;
    set_status("тест: DNS/HTTP...");

    /* 1) HTTP connectivity check (как Android generate_204) */
    int64_t t0 = esp_timer_get_time();
    esp_http_client_config_t cfg = {
        .url = "http://connectivitycheck.gstatic.com/generate_204",
        .timeout_ms = 8000,
        .crt_bundle_attach = NULL,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        strlcpy(out->note, "http init fail", sizeof(out->note));
        set_status("тест: ошибка HTTP");
        s.state = WIFI_MGR_FAIL;
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    int64_t t1 = esp_timer_get_time();
    out->latency_ms = (uint32_t)((t1 - t0) / 1000);
    out->http_status = status;

    if (err != ESP_OK || (status != 204 && status != 200)) {
        snprintf(out->note, sizeof(out->note), "нет сети (HTTP %d)", status);
        set_status(out->note);
        out->ok = false;
        s.state = WIFI_MGR_FAIL;
        return ESP_FAIL;
    }

    set_status("тест: замер скорости...");
    /* 2) Короткий speed_test — «чем занять» и реальная полоса */
    speed_result_t spd;
    if (speed_test_run(&spd) == ESP_OK) {
        out->mbps = spd.mbps;
        snprintf(out->note, sizeof(out->note), "OK %.1f Мбит/с, %u мс",
                 (double)spd.mbps, (unsigned)out->latency_ms);
    } else {
        out->mbps = 0;
        snprintf(out->note, sizeof(out->note), "HTTP OK, speed fail (%u мс)",
                 (unsigned)out->latency_ms);
    }
    out->ok = true;
    set_status(out->note);
    s.state = WIFI_MGR_CONNECTED;
    return ESP_OK;
}

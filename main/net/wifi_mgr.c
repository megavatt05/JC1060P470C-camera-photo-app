/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi‑Fi менеджер CamBrowser (P4 + C6 / esp_hosted + esp_wifi_remote).
 *
 * Паттерн как в официальных примерах ESP-IDF:
 *  - examples/wifi/getting_started/station  (event group, retry, STA_START→connect)
 *  - examples/wifi/softap_sta               (APSTA, оба netif, один wifi_start)
 *  - network_provisioning / SoftAP portal  (нет кредов → портал, есть → STA)
 *
 * API для LCD: скан, connect, SoftAP http://192.168.4.1:8080/, тест интернета.
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
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "net/wifi_mgr.h"
#include "net/speed_test.h"

static const char *TAG = "wifi_mgr";

#define NVS_NS           "wifi_mgr"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASS     "pass"
#define PORTAL_SSID      "CamBrowser-Setup"
#define PORTAL_HTTP_PORT 8080
#define WIFI_MAXIMUM_RETRY  8
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static struct {
    bool inited;
    bool want_connect;          /* STA_START → esp_wifi_connect() только если true */
    volatile wifi_mgr_state_t state;
    char status[48];
    char ip[16];
    char saved_ssid[WIFI_MGR_SSID_MAX + 1];
    char saved_pass[WIFI_MGR_PASS_MAX + 1];
    wifi_mgr_ap_t aps[WIFI_MGR_SCAN_MAX];
    int ap_count;
    int retry;
    EventGroupHandle_t eg;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    httpd_handle_t portal_httpd;
    bool handlers_reg;
} s;

static void set_status(const char *msg)
{
    strlcpy(s.status, msg, sizeof(s.status));
    ESP_LOGI(TAG, "%s", msg);
}

/* ---------- NVS (как provisioning: креды переживают reboot) ---------- */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
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
        ESP_LOGI(TAG, "NVS save SSID=\"%s\"", s.saved_ssid);
    }
    return err;
}

/* ---------- Event handler (паттерн station_example_main.c) ---------- */

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Официальный пример: connect только после START и если есть цель */
        if (s.want_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s.ip[0] = '\0';
        int reason = 0;
        if (data) {
            wifi_event_sta_disconnected_t *d = data;
            reason = d->reason;
            ESP_LOGW(TAG, "disconnect ssid=%s reason=%d rssi=%d",
                     (const char *)d->ssid, reason, (int)d->rssi);
        }
        if (!s.want_connect) {
            return;
        }
        if (s.retry < WIFI_MAXIMUM_RETRY) {
            s.retry++;
            char buf[40];
            snprintf(buf, sizeof(buf), "wifi: retry %d/%d", s.retry, WIFI_MAXIMUM_RETRY);
            set_status(buf);
            s.state = WIFI_MGR_CONNECTING;
            esp_wifi_connect();
        } else {
            s.state = WIFI_MGR_FAIL;
            if (reason == 202 || reason == 15) {
                set_status("wifi: неверный пароль?");
            } else if (reason == 201) {
                set_status("wifi: сеть не найдена");
            } else if (reason == 203) {
                set_status("wifi: assoc fail");
            } else {
                set_status("wifi: нет связи");
            }
            if (s.eg) {
                xEventGroupSetBits(s.eg, WIFI_FAIL_BIT);
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        set_status("портал: 192.168.4.1:8080");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s.ip, sizeof(s.ip), IPSTR, IP2STR(&e->ip_info.ip));
        s.retry = 0;
        s.state = WIFI_MGR_CONNECTED;
        set_status("wifi: есть IP");
        ESP_LOGI(TAG, "got ip: %s", s.ip);
        /* softap_sta: STA = default interface для исходящего трафика */
        if (s.sta_netif) {
            esp_netif_set_default_netif(s.sta_netif);
            ESP_LOGI(TAG, "default netif = STA");
        }
        if (s.eg) {
            xEventGroupSetBits(s.eg, WIFI_CONNECTED_BIT);
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
    ESP_ERROR_CHECK(err);

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
    if (s.eg == NULL) {
        s.eg = xEventGroupCreate();
    }

    static bool wifi_inited;
    if (!wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        wifi_inited = true;
    }

    if (!s.handlers_reg) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));
        s.handlers_reg = true;
    }
    return ESP_OK;
}

/* ---------- Public API ---------- */

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

    /* Есть сохранённые креды → STA connect (паттерн station example) */
    if (s.saved_ssid[0]) {
        return wifi_mgr_connect(s.saved_ssid, s.saved_pass);
    }
    /* Kconfig, если не placeholder */
    if (strlen(CONFIG_EB_WIFI_SSID) > 0 &&
        strcmp(CONFIG_EB_WIFI_SSID, "myssid") != 0) {
        return wifi_mgr_connect(CONFIG_EB_WIFI_SSID, CONFIG_EB_WIFI_PASSWORD);
    }

    /* Нет кредов: STA idle, без connect (не цепляться к хвосту NVS esp_wifi) */
    s.want_connect = false;
    wifi_config_t empty = { 0 };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &empty));
    ESP_ERROR_CHECK(esp_wifi_start());
    s.state = WIFI_MGR_IDLE;
    set_status("wifi: нет сети — ПОРТАЛ или СКАН");
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
    ESP_ERROR_CHECK(ensure_stack());
    s.state = WIFI_MGR_SCANNING;
    s.ap_count = 0;
    set_status("wifi: сканирование...");

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) {
        s.want_connect = false;
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
    esp_err_t err = esp_wifi_scan_start(&sc, true);
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
    ESP_ERROR_CHECK(ensure_stack());

    const char *pass = password;
    if (pass == NULL && s.saved_ssid[0] && strcmp(ssid, s.saved_ssid) == 0) {
        pass = s.saved_pass;
    }

    s.want_connect = true;
    s.retry = 0;
    s.ip[0] = '\0';
    s.state = WIFI_MGR_CONNECTING;
    set_status("wifi: подключение...");
    if (s.eg) {
        xEventGroupClearBits(s.eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    /* Как в station example: mode → config → start → (STA_START → connect) */
    wifi_config_t wcfg = { 0 };
    strlcpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid));
    if (pass) {
        strlcpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    }
    wcfg.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;

    /* Портал мог оставить APSTA — переводим в чистый STA */
    if (s.portal_httpd) {
        httpd_stop(s.portal_httpd);
        s.portal_httpd = NULL;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    /* Если уже был START — connect вручную */
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s.eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        nvs_save(ssid, pass ? pass : "");
        return ESP_OK;
    }
    s.state = WIFI_MGR_FAIL;
    if (!(bits & WIFI_FAIL_BIT)) {
        set_status("wifi: таймаут");
    }
    return ESP_ERR_TIMEOUT;
#endif
}

/* ---------- SoftAP портал (softap_sta + HTTP form) ---------- */

static esp_err_t portal_root_get(httpd_req_t *req)
{
    static const char *html =
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>CamBrowser Wi-Fi</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:16px}"
        "h1{color:#6cf}input,button{font-size:1rem;padding:10px;margin:6px 0;width:100%;max-width:320px}"
        "button{background:#06c;color:#fff;border:0;border-radius:8px}"
        "</style></head><body>"
        "<h1>Настройка Wi-Fi</h1>"
        "<p>Только <b>2.4 ГГц</b>. После сохранения плата подключится к сети.</p>"
        "<form method=POST action=/save>"
        "<label>SSID</label><input name=ssid required maxlength=32>"
        "<label>Пароль</label><input name=pass type=password maxlength=64>"
        "<button type=submit>Сохранить и подключить</button>"
        "</form></body></html>";
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
    char ssid[WIFI_MGR_SSID_MAX + 1] = { 0 };
    char pass[WIFI_MGR_PASS_MAX + 1] = { 0 };
    char *p = strstr(buf, "ssid=");
    if (p) {
        p += 5;
        char *e = strchr(p, '&');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > WIFI_MGR_SSID_MAX) len = WIFI_MGR_SSID_MAX;
        memcpy(ssid, p, len);
        for (char *q = ssid; *q; q++) if (*q == '+') *q = ' ';
    }
    p = strstr(buf, "pass=");
    if (p) {
        p += 5;
        char *e = strchr(p, '&');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > WIFI_MGR_PASS_MAX) len = WIFI_MGR_PASS_MAX;
        memcpy(pass, p, len);
        for (char *q = pass; *q; q++) if (*q == '+') *q = ' ';
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body style='background:#111;color:#eee'>"
                            "<h2>Сохранено</h2><p>Подключаюсь...</p></body></html>");
    if (ssid[0]) {
        nvs_save(ssid, pass);
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
    ESP_ERROR_CHECK(ensure_stack());
    s.want_connect = false; /* не долбить connect во время AP */

    if (s.ap_netif == NULL) {
        s.ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, PORTAL_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(PORTAL_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    /* softap_sta: APSTA, оба конфига, затем start */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (s.portal_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = PORTAL_HTTP_PORT;
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

esp_err_t wifi_mgr_test_internet(wifi_mgr_test_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    s.state = WIFI_MGR_TESTING;
    set_status("тест: HTTP...");

    int64_t t0 = esp_timer_get_time();
    esp_http_client_config_t cfg = {
        .url = "http://connectivitycheck.gstatic.com/generate_204",
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        strlcpy(out->note, "http init fail", sizeof(out->note));
        s.state = WIFI_MGR_FAIL;
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    out->latency_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    out->http_status = status;

    if (err != ESP_OK || (status != 204 && status != 200)) {
        snprintf(out->note, sizeof(out->note), "нет сети (HTTP %d)", status);
        set_status(out->note);
        out->ok = false;
        s.state = WIFI_MGR_FAIL;
        return ESP_FAIL;
    }

    set_status("тест: скорость...");
    speed_result_t spd;
    if (speed_test_run(&spd) == ESP_OK) {
        out->mbps = spd.mbps;
        snprintf(out->note, sizeof(out->note), "OK %.1f Мбит/с, %u мс",
                 (double)spd.mbps, (unsigned)out->latency_ms);
    } else {
        snprintf(out->note, sizeof(out->note), "HTTP OK (%u мс)", (unsigned)out->latency_ms);
    }
    out->ok = true;
    set_status(out->note);
    s.state = WIFI_MGR_CONNECTED;
    return ESP_OK;
}

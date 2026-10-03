/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi-Fi STA на JC1060P470C_I_W_Y: ESP32-P4 + ESP32-C6 через esp_hosted.
 *
 * Важно:
 *  - P4 без радио; C6 — сопроцессор (Wi-Fi 6 / 2.4 ГГц).
 *  - SDIO C6: типично CLK=18 CMD=19 D0–D3=14–17 RST=54 (дефолты hosted).
 *  - microSD на этой плате может делить линии с C6 — при активном Wi-Fi
 *    SD лучше не монтировать одновременно (см. docs/WIFI_C6.md).
 *  - Порядок: netif + event loop → wifi_init → STA config → connect.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "net/app_wifi.h"

static const char *TAG = "app_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static struct {
    bool started;
    volatile app_wifi_state_t state;
    char ip_str[16];
    char reason[40];
    int retry;
    EventGroupHandle_t eg;
    esp_netif_t *netif;
} s_w;

static void set_reason(const char *s)
{
    strlcpy(s_w.reason, s, sizeof(s_w.reason));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_STA_START) {
        set_reason("wifi: connecting...");
        s_w.state = APP_WIFI_CONNECTING;
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_w.state = APP_WIFI_CONNECTING;
        s_w.ip_str[0] = '\0';
        if (s_w.retry < 12) {
            s_w.retry++;
            set_reason("wifi: reconnect...");
            esp_wifi_connect();
        } else {
            set_reason("wifi: failed");
            if (s_w.eg) {
                xEventGroupSetBits(s_w.eg, WIFI_FAIL_BIT);
            }
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_w.ip_str, sizeof(s_w.ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        s_w.retry = 0;
        s_w.state = APP_WIFI_GOT_IP;
        set_reason("wifi: got ip");
        ESP_LOGI(TAG, "Got IP: %s", s_w.ip_str);
        if (s_w.eg) {
            xEventGroupSetBits(s_w.eg, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t app_wifi_start(void)
{
#if !CONFIG_EB_WIFI_ENABLE
    set_reason("wifi: disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_w.started) {
        return ESP_OK;
    }
    s_w.started = true;
    s_w.state = APP_WIFI_CONNECTING;
    set_reason("wifi: starting...");
    s_w.ip_str[0] = '\0';
    s_w.retry = 0;
    s_w.eg = xEventGroupCreate();

    /* NVS нужен стеку Wi-Fi (калибровки / конфиг) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        set_reason("wifi: nvs fail");
        return err;
    }

    /* netif / event loop могут уже быть созданы app_eth_start() */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop: %s", esp_err_to_name(err));
        return err;
    }

    s_w.netif = esp_netif_create_default_wifi_sta();
    if (s_w.netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        set_reason("wifi: netif fail");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s (проверьте esp_hosted + slave=c6)",
                 esp_err_to_name(err));
        set_reason("wifi: init fail");
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &on_ip_event, NULL));

    wifi_config_t wcfg = { 0 };
    strlcpy((char *)wcfg.sta.ssid, CONFIG_EB_WIFI_SSID, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, CONFIG_EB_WIFI_PASSWORD,
            sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA start, SSID=\"%s\" (C6 via esp_hosted)", CONFIG_EB_WIFI_SSID);
    set_reason("wifi: connecting...");
    return ESP_OK;
#endif
}

bool app_wifi_ready(void)
{
    return s_w.state == APP_WIFI_GOT_IP;
}

const char *app_wifi_ip_str(void)
{
    return s_w.ip_str;
}

const char *app_wifi_status_str(void)
{
    return s_w.reason[0] ? s_w.reason : "wifi: —";
}

app_wifi_state_t app_wifi_state(void)
{
    return s_w.state;
}

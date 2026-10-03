/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi‑Fi менеджер с нуля для JC1060P470C (ESP32‑P4 + C6).
 * Стек: esp_hosted + esp_wifi_remote. API совместим с экраном browser.
 *
 * Паттерн: ESP-IDF examples/wifi/getting_started/station + softap_sta.
 */

#ifndef NET_WIFI_MGR_H
#define NET_WIFI_MGR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MGR_SSID_MAX  32
#define WIFI_MGR_PASS_MAX  64
#define WIFI_MGR_SCAN_MAX  16

typedef struct {
    char    ssid[WIFI_MGR_SSID_MAX + 1];
    int8_t  rssi;
    uint8_t authmode;
    bool    is_open;
} wifi_mgr_ap_t;

typedef enum {
    WIFI_MGR_IDLE = 0,
    WIFI_MGR_SCANNING,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_AP_MODE,
    WIFI_MGR_TESTING,
    WIFI_MGR_FAIL,
} wifi_mgr_state_t;

typedef struct {
    bool     ok;
    int      http_status;
    uint32_t latency_ms;
    float    mbps;
    char     note[48];
} wifi_mgr_test_t;

esp_err_t wifi_mgr_init(void);

wifi_mgr_state_t wifi_mgr_state(void);
const char *wifi_mgr_status_str(void);
const char *wifi_mgr_ip_str(void);
const char *wifi_mgr_saved_ssid(void);
bool wifi_mgr_ready(void);

esp_err_t wifi_mgr_scan(void);
int wifi_mgr_scan_count(void);
const wifi_mgr_ap_t *wifi_mgr_scan_get(int index);

/** password == NULL → пароль из NVS (если SSID совпадает) */
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);

esp_err_t wifi_mgr_start_portal(void);
esp_err_t wifi_mgr_stop_portal(void);

esp_err_t wifi_mgr_test_internet(wifi_mgr_test_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NET_WIFI_MGR_H */

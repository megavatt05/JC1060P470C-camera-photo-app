/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi‑Fi менеджер CamBrowser (P4 + C6 / esp_hosted):
 *  - скан сетей, подключение, сохранение SSID/пароля в NVS;
 *  - SoftAP «CamBrowser-Setup» + веб-страница настройки;
 *  - тест интернета (HTTP generate_204 + опционально speed_test).
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

#define WIFI_MGR_SSID_MAX   32
#define WIFI_MGR_PASS_MAX   64
#define WIFI_MGR_SCAN_MAX   16

typedef struct {
    char     ssid[WIFI_MGR_SSID_MAX + 1];
    int8_t   rssi;
    uint8_t  authmode;   /* wifi_auth_mode_t */
    bool     is_open;
} wifi_mgr_ap_t;

typedef enum {
    WIFI_MGR_IDLE = 0,
    WIFI_MGR_SCANNING,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_AP_MODE,       /* SoftAP портал активен */
    WIFI_MGR_TESTING,       /* идёт тест интернета */
    WIFI_MGR_FAIL,
} wifi_mgr_state_t;

typedef struct {
    bool     ok;
    int      http_status;   /* 204 = отлично */
    uint32_t latency_ms;
    float    mbps;          /* 0 если speed не запускали */
    char     note[48];
} wifi_mgr_test_t;

/** Загрузка NVS + старт STA (если есть сохранённые данные) или только стек */
esp_err_t wifi_mgr_init(void);

wifi_mgr_state_t wifi_mgr_state(void);
const char *wifi_mgr_status_str(void);
const char *wifi_mgr_ip_str(void);
bool wifi_mgr_ready(void);

/** Сохранённый SSID из NVS (может быть пустым) */
const char *wifi_mgr_saved_ssid(void);

/** Скан (блокирует до ~4 с). Результаты в wifi_mgr_scan_get */
esp_err_t wifi_mgr_scan(void);
int wifi_mgr_scan_count(void);
const wifi_mgr_ap_t *wifi_mgr_scan_get(int index);

/** Подключиться и сохранить в NVS при успехе */
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);

/** SoftAP + веб-страница http://192.168.4.1/ */
esp_err_t wifi_mgr_start_portal(void);
esp_err_t wifi_mgr_stop_portal(void);

/**
 * Тест интернета: HTTP GET generate_204, затем короткий speed_test.
 * Блокирует 5–20 с; прогресс в status_str.
 */
esp_err_t wifi_mgr_test_internet(wifi_mgr_test_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NET_WIFI_MGR_H */

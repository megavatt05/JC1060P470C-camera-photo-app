/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Wi-Fi STA через ESP32-C6 (esp_hosted / esp_wifi_remote) на JC1060P470C.
 * P4 сам радио не имеет — только C6 по SDIO.
 */

#ifndef APP_WIFI_H
#define APP_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WIFI_DOWN = 0,
    APP_WIFI_CONNECTING,
    APP_WIFI_GOT_IP,
} app_wifi_state_t;

/** @brief Инит NVS (если нужно), esp_hosted/Wi-Fi remote, STA + DHCP.
 *  Вызывать после/вместе с общим netif; безопасен при повторном вызове. */
esp_err_t app_wifi_start(void);

bool app_wifi_ready(void);
const char *app_wifi_ip_str(void);
const char *app_wifi_status_str(void);
app_wifi_state_t app_wifi_state(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WIFI_H */

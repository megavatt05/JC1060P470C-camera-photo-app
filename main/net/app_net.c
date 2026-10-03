/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Агрегация Ethernet (IP101) + Wi-Fi (C6) для CamBrowser.
 */

#include "esp_log.h"
#include "sdkconfig.h"
#include "net/app_eth.h"
#include "net/app_wifi.h"
#include "net/wifi_mgr.h"
#include "net/app_net.h"

static const char *TAG = "app_net";

esp_err_t app_net_start(void)
{
    esp_err_t eth_err = ESP_OK;
    esp_err_t wifi_err = ESP_OK;

#if CONFIG_EB_ETH_ENABLE
    eth_err = app_eth_start();
    if (eth_err != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet: %s", esp_err_to_name(eth_err));
    }
#else
    ESP_LOGI(TAG, "Ethernet выключен (CONFIG_EB_ETH_ENABLE=n)");
#endif

#if CONFIG_EB_WIFI_ENABLE
    wifi_err = wifi_mgr_init();  /* скан/NVS/портал: wifi_mgr */
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi: %s", esp_err_to_name(wifi_err));
    }
#else
    ESP_LOGI(TAG, "Wi-Fi выключен (CONFIG_EB_WIFI_ENABLE=n)");
#endif

#if CONFIG_EB_ETH_ENABLE && CONFIG_EB_WIFI_ENABLE
    return (eth_err == ESP_OK || wifi_err == ESP_OK) ? ESP_OK : eth_err;
#elif CONFIG_EB_WIFI_ENABLE
    return wifi_err;
#else
    return eth_err;
#endif
}

bool app_net_ready(void)
{
    bool ok = false;
#if CONFIG_EB_ETH_ENABLE
    ok = ok || app_eth_ready();
#endif
#if CONFIG_EB_WIFI_ENABLE
    ok = ok || wifi_mgr_ready() || app_wifi_ready();
#endif
    return ok;
}

const char *app_net_ip_str(void)
{
#if CONFIG_EB_ETH_ENABLE
    if (app_eth_ready() && app_eth_ip_str()[0]) {
        return app_eth_ip_str();
    }
#endif
#if CONFIG_EB_WIFI_ENABLE
    if (wifi_mgr_ready() && wifi_mgr_ip_str()[0]) {
        return wifi_mgr_ip_str();
    }
    if (app_wifi_ready() && app_wifi_ip_str()[0]) {
        return app_wifi_ip_str();
    }
#endif
    return "";
}

const char *app_net_status_str(void)
{
#if CONFIG_EB_ETH_ENABLE
    if (app_eth_ready()) {
        return app_eth_status_str();
    }
#endif
#if CONFIG_EB_WIFI_ENABLE
    if (app_wifi_ready()) {
        return app_wifi_status_str();
    }
#if CONFIG_EB_ETH_ENABLE
    if (!app_eth_ready()) {
        return app_wifi_status_str();
    }
#else
    return app_wifi_status_str();
#endif
#endif
#if CONFIG_EB_ETH_ENABLE
    return app_eth_status_str();
#else
    return "нет сети";
#endif
}

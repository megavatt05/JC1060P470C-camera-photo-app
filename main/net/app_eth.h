/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Ethernet (IP101 на внутреннем EMAC / RMII) + DHCP для CamBrowser.
 */

#ifndef APP_ETH_H
#define APP_ETH_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_ETH_DOWN = 0,       /* драйвер не запущен или кабель отключён */
    APP_ETH_LINK_UP,        /* link PHY есть, ждём DHCP */
    APP_ETH_GOT_IP,         /* DHCP готов, сеть доступна */
} app_eth_state_t;

/**
 * @brief Инициализация EMAC + IP101 PHY, netif, старт драйвера и DHCP.
 *
 * Повторный вызов — no-op, возвращает ESP_OK.
 * Пины из Kconfig (EB_ETH_*): MDC 31, MDIO 52, REF_CLK in 50, RST 51, addr 1.
 *
 * @return ESP_OK при успехе
 */
esp_err_t app_eth_start(void);

/** @brief true после получения IP (браузер может выходить в сеть) */
bool app_eth_ready(void);

/** @brief Текущий IPv4 ("192.168.1.42"), пустая строка до APP_ETH_GOT_IP */
const char *app_eth_ip_str(void);

/** @brief Краткий статус для статус-бара */
const char *app_eth_status_str(void);

app_eth_state_t app_eth_state(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ETH_H */

/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser network bring-up API (W5500 SPI Ethernet + DHCP).
 */

#ifndef APP_ETH_H
#define APP_ETH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_ETH_DOWN = 0,       /* driver not started or cable detached */
    APP_ETH_LINK_UP,        /* PHY link detected, waiting for DHCP  */
    APP_ETH_GOT_IP,         /* DHCP done, network ready             */
} app_eth_state_t;

/**
 * @brief Initialize SPI, W5500, netif and start the driver + DHCP.
 *
 * Safe to call once; subsequent calls are no-ops returning ESP_OK.
 * All pins/parameters come from Kconfig (EB_W5500_*).
 *
 * @return ESP_OK on success
 */
esp_err_t app_eth_start(void);

/** @brief True once an IP address was obtained (browser may go online) */
bool app_eth_ready(void);

/** @brief String form of the current IPv4 address ("192.168.1.42"),
 *         empty until APP_ETH_GOT_IP */
const char *app_eth_ip_str(void);

/** @brief Short human status for the status bar ("cable detached",
 *         "link up, dhcp...", "got ip", "starting...") */
const char *app_eth_status_str(void);

app_eth_state_t app_eth_state(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ETH_H */

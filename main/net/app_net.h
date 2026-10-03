/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Единый статус сети для UI: Ethernet и/или Wi-Fi.
 */

#ifndef APP_NET_H
#define APP_NET_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Поднять Ethernet (если включён) и/или Wi-Fi. */
esp_err_t app_net_start(void);

/** @brief Есть IP хотя бы на одном интерфейсе. */
bool app_net_ready(void);

/** @brief IP для статус-бара (предпочтение Ethernet, иначе Wi-Fi). */
const char *app_net_ip_str(void);

/** @brief Краткая строка статуса для UI. */
const char *app_net_status_str(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_NET_H */

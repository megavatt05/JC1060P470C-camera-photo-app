/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser log server: ESP_LOG tail over HTTP, see log_server.c.
 *
 * Call log_server_start() once the network is up (from the got-IP event
 * handler). Idempotent and safe to call again at any time.
 */

#ifndef NET_LOG_SERVER_H
#define NET_LOG_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Hook the log sink into the ring buffer and serve
 *         http://<board-ip>/log. Idempotent. */
esp_err_t log_server_start(void);

/** Остановить HTTP (освободить порт 80 под SoftAP-портал). */
esp_err_t log_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_LOG_SERVER_H */

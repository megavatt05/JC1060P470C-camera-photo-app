/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser web client API.
 */

#ifndef WEB_CLIENT_H
#define WEB_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTPS GET a document into a PSRAM buffer.
 *
 * TLS certificate verification is always enabled (x509 bundle).
 * The body is truncated at CONFIG_EB_MAX_BODY_KB.
 *
 * @param[in]  url         Absolute https:// URL
 * @param[in]  extra_cookie  Optional "Cookie" header (NULL = none)
 * @param[out] out_body    NUL-terminated body (caller frees with free())
 * @param[out] out_len     Body length in bytes
 * @param[out] out_status  Optional HTTP status (may be NULL)
 * @return ESP_OK on any response actually received (check *out_status),
 *         transport errors otherwise
 */
esp_err_t web_get(const char *url, const char *extra_cookie,
                  char **out_body, size_t *out_len, int *out_status);

/**
 * @brief Build and GET a search query.
 *
 * @param use_google false = DuckDuckGo html endpoint (default engine),
 *                   true  = Google SERP (best-effort)
 * @return same contract as web_get()
 */
esp_err_t web_search(const char *query, bool use_google,
                     char **out_body, size_t *out_len);

/** @brief Decode %XX and '+' escapes (size of dst must cover the result) */
bool web_url_decode(const char *src, char *dst, size_t dst_size);

/** @brief Percent-encode a query-string component; returns encoded length */
size_t web_url_encode(const char *src, char *dst, size_t dst_size);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CLIENT_H */

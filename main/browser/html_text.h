/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser HTML processing API.
 */

#ifndef HTML_TEXT_H
#define HTML_TEXT_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_RESULT_URL_MAX    256
#define WEB_RESULT_TITLE_MAX  160
#define WEB_RESULTS_MAX       20

typedef struct {
    char url[WEB_RESULT_URL_MAX];
    char title[WEB_RESULT_TITLE_MAX];
} web_result_t;

/** @brief Extract the <title> text of a document (ASCII-rendered) */
esp_err_t html_extract_title(char *html, char *out, size_t out_size);

/**
 * @brief Parse a DuckDuckGo html-endpoint SERP into a result list.
 *
 * The buffer is modified in place (blocks stripped). Returns the number
 * of results collected (0 = none / probable antibot page).
 */
int html_parse_serp_ddg(char *html, web_result_t *results, int max_results);

/** @brief Best-effort Google SERP parser (same contract as above) */
int html_parse_serp_google(char *html, web_result_t *results, int max_results);

/**
 * @brief Convert a full page to plain ASCII text (tags stripped, entities
 *        decoded, block boundaries -> newlines). Buffer is modified in place.
 */
esp_err_t html_to_text(char *html, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* HTML_TEXT_H */

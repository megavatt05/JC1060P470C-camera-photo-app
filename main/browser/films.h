/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser "ФИЛЬМЫ": public-domain feature films from archive.org.
 *
 * Two network steps, both plain HTTPS GET (web_client):
 *   1. advancedsearch.php -> JSON list of {identifier, title}
 *   2. metadata/<id>      -> JSON file list -> best mp4 derivative
 *
 * Before handing the URL to the player, films_probe_url() range-reads the
 * MP4 headers and walks the boxes down to avcC to check the H.264 profile:
 * the SW decoder (tinyh264) only accepts Constrained/Baseline (profile 66).
 * Main/High streams are rejected here with a readable reason instead of a
 * decoder error mid-playback. The canonical archive.org/download/... URL is
 * given to the player as-is: esp_gmf_io_http follows 301/302 itself.
 */

#ifndef FILMS_H
#define FILMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FILMS_MAX       20      /* search results kept          */
#define FILM_TITLE_MAX  64
#define FILM_IDENT_MAX  48
#define FILMS_URL_MAX   224     /* play URL buffer in the caller (fits
                                 * media_player's MEDIA_URL_MAX=256) */

typedef struct {
    char title[FILM_TITLE_MAX];
    char ident[FILM_IDENT_MAX];
} film_item_t;

typedef enum {
    FILMS_PLAY_OK = 0,
    FILMS_ERR_NET,          /* search/metadata/probe transport failure */
    FILMS_ERR_NO_RESULT,
    FILMS_ERR_NO_MP4,       /* item has no .mp4 derivative             */
    FILMS_ERR_NO_RANGE,     /* server ignored the Range request        */
    FILMS_ERR_NOT_H264,     /* HEVC / MPEG-4 ASP / unknown codec       */
    FILMS_ERR_HIGH_PROFILE, /* H.264 Main/High - decoder rejects it    */
    FILMS_ERR_NO_VIDEO,     /* mp4 without a video track               */
} films_play_err_t;

/**
 * @brief Search the archive.org feature_films collection.
 *
 * Empty query = "popular": the collection sorted by downloads.
 * Titles are decoded from JSON escapes to UTF-8.
 *
 * @return ESP_OK with *out_n >= 0 (0 = nothing found), ESP_FAIL on network
 *         errors (out_n set to 0)
 */
esp_err_t films_search(const char *query, film_item_t *items, int max_items,
                       int *out_n);

/**
 * @brief Pick the best mp4 derivative of an item and build its URL.
 *
 * Prefers the classic "_512kb.mp4" derivative, else the smallest .mp4.
 * The returned URL is the canonical https://archive.org/download/<id>/<file>
 * - the player's HTTP element follows archive.org's redirect to the node.
 */
films_play_err_t films_resolve(const char *ident, char *url, size_t urlsz);

/**
 * @brief Check that a remote MP4 is decodable by the SW H264 decoder.
 *
 * Range-reads the top-level MP4 boxes (and the whole moov, wherever it
 * sits), finds the first video track's sample entry and its avcC record.
 *
 * @param[out] out_w  decoded video width (0 if unknown)
 * @param[out] out_h  decoded video height (0 if unknown)
 */
films_play_err_t films_probe_url(const char *url, uint16_t *out_w, uint16_t *out_h);

/** @brief Russian one-liner for a films_play_err_t (for the status line) */
const char *films_err_str(films_play_err_t e);

#ifdef __cplusplus
}
#endif

#endif /* FILMS_H */

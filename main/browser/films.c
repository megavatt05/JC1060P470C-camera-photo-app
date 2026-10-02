/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser "ФИЛЬМЫ": archive.org catalog + on-device H.264 profile gate.
 * See films.h for the API contract.
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "browser/web_client.h"
#include "browser/films.h"

static const char *TAG = "films";

/* Sanity cap for the moov box size taken from its header. The probe never
 * downloads the moov wholesale any more (lazy window walk below), so this
 * is only a guard against garbage headers, not a download budget - 64 MB
 * covers any real index (a 2 h film carries a 3-5 MB moov). */
#define MOOV_SANITY_MAX (64u << 20)
/* Sliding read window for the box walk: headers of the descent path plus
 * the stsd cluster live within a few dozen KB around each box, so one
 * 64 KB Range read usually serves the whole video-trak descent. */
#define PROBE_WINDOW    (64 * 1024)

/* progress callback (see films.h): lets the UI animate a busy card while
 * these calls block for seconds to minutes */
static films_prog_fn s_prog_fn;
static void         *s_prog_arg;

void films_set_prog_cb(films_prog_fn fn, void *ctx)
{
    s_prog_fn  = fn;
    s_prog_arg = ctx;
}

static void prog(films_prog_t st, uint32_t done, uint32_t total)
{
    if (s_prog_fn != NULL) {
        s_prog_fn(s_prog_arg, st, done, total);
    }
}

/* ------------------------------------------------------------------ */
/* Minimal JSON string extraction (archive.org responses only)        */
/* ------------------------------------------------------------------ */

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* UTF-8 encode cp into out, return byte count (max 4) */
static int utf8_enc(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode the JSON string starting at the opening quote into out.
 * Returns pointer past the closing quote, NULL on malformed input. */
static const char *json_decode_str(const char *p, char *out, size_t sz)
{
    size_t o = 0;
    if (*p != '"') {
        return NULL;
    }
    p++;
    while (*p != '\0' && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1] != '\0') {
            p++;
            switch (*p) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                for (int i = 1; i <= 4; i++) {
                    int h = hexv(p[i]);
                    if (h < 0) {
                        return NULL;
                    }
                    cp = (cp << 4) | (uint32_t)h;
                }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                    uint32_t lo = 0;
                    int ok = 1;
                    for (int i = 3; i <= 6; i++) {
                        int h = hexv(p[i]);
                        if (h < 0) { ok = 0; break; }
                        lo = (lo << 4) | (uint32_t)h;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                char tmp[4];
                int n = utf8_enc(cp, tmp);
                if (o + n >= sz) {
                    return NULL;
                }
                memcpy(out + o, tmp, n);
                o += n;
                p++;
                continue;
            }
            default:
                return NULL;
            }
        }
        if (o + 1 >= sz) {
            return NULL;    /* truncate: caller gets a partial value */
        }
        out[o++] = c;
        p++;
    }
    if (*p != '"') {
        return NULL;
    }
    out[o] = '\0';
    return p + 1;
}

/* Find "key" and decode its string value (or the first element of an
 * array value). Returns a pointer just past the value or NULL. */
static const char *json_str_after(const char *hay, const char *key,
                                  char *out, size_t sz)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    out[0] = '\0';
    const char *p = hay;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == ':') {
            p++;
        }
        if (*p == '[') {            /* take the first array element */
            p++;
            while (*p != '\0' && *p != '"') {
                p++;
            }
        }
        if (*p != '"') {
            continue;               /* not a string value, keep looking */
        }
        return json_decode_str(p, out, sz);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* archive.org: search + metadata                                     */
/* ------------------------------------------------------------------ */

static void search_url(const char *query, char *url, size_t sz)
{
    if (query[0] == '\0') {
        /* "популярное": whole collection by download count */
        snprintf(url, sz,
                 "https://archive.org/advancedsearch.php"
                 "?q=collection%%3A%%28feature_films%%29"
                 "&fl%%5B%%5D=identifier&fl%%5B%%5D=title"
                 "&sort%%5B%%5D=-downloads&rows=20&page=1&output=json");
        return;
    }
    char q[160];
    web_url_encode(query, q, sizeof(q));
    snprintf(url, sz,
             "https://archive.org/advancedsearch.php"
             "?q=collection%%3A%%28feature_films%%29%%20AND%%20title%%3A%%28%s%%29"
             "&fl%%5B%%5D=identifier&fl%%5B%%5D=title&rows=20&page=1&output=json",
             q);
}

static bool ident_ok(const char *s)
{
    for (const char *c = s; *c != '\0'; c++) {
        char ch = *c;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return s[0] != '\0';
}

esp_err_t films_search(const char *query, film_item_t *items, int max_items,
                       int *out_n)
{
    *out_n = 0;
    if (items == NULL || max_items <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[640];
    search_url(query, url, sizeof(url));

    prog(FILMS_PROG_SEARCH, 0, 0);
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = web_get(url, NULL, &body, &len, NULL);
    if (err != ESP_OK || body == NULL) {
        ESP_LOGW(TAG, "search fetch failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    const char *p = body;
    while (*out_n < max_items &&
           (p = strstr(p, "\"identifier\"")) != NULL) {
        const char *next = strstr(p + 1, "\"identifier\"");

        char ident[FILM_IDENT_MAX];
        if (json_str_after(p, "identifier", ident, sizeof(ident)) == NULL ||
            !ident_ok(ident)) {
            p = (next != NULL) ? next : p + 12;
            continue;
        }
        /* title lives somewhere in the same doc object */
        size_t window = (next != NULL) ? (size_t)(next - p) : strlen(p);
        char title[FILM_TITLE_MAX] = "(без названия)";
        char tbuf[FILM_TITLE_MAX];
        char *doc = malloc(window + 1);
        if (doc != NULL) {
            memcpy(doc, p, window);
            doc[window] = '\0';
            if (json_str_after(doc, "title", tbuf, sizeof(tbuf)) != NULL &&
                tbuf[0] != '\0') {
                strlcpy(title, tbuf, sizeof(title));
            }
            free(doc);
        }

        strlcpy(items[*out_n].ident, ident, sizeof(items[*out_n].ident));
        strlcpy(items[*out_n].title, title, sizeof(items[*out_n].title));
        (*out_n)++;
        p = (next != NULL) ? next : p + window;
    }

    free(body);
    ESP_LOGI(TAG, "search '%s': %d hits", query, *out_n);
    return ESP_OK;
}

/* scan metadata JSON: every files[] entry contributes name+size */
typedef void (*file_cb_t)(const char *name, uint64_t size, void *ctx);

static void metadata_scan_files(const char *body, file_cb_t cb, void *ctx)
{
    const char *p = body;
    while ((p = strstr(p, "\"name\"")) != NULL) {
        char name[128];
        const char *after = NULL;
        const char *v = strchr(p, ':');
        if (v != NULL) {
            v++;
            while (*v == ' ') {
                v++;
            }
            if (*v == '[') {
                v++;
                while (*v != '\0' && *v != '"') {
                    v++;
                }
            }
            after = json_decode_str(v, name, sizeof(name));
        }
        if (after == NULL) {
            p += 6;
            continue;
        }

        /* size sits before the next "name" key (or end of the files array) */
        const char *next = strstr(after, "\"name\"");
        uint64_t size = 0;
        const char *s = strstr(after, "\"size\"");
        if (s != NULL && (next == NULL || s < next)) {
            const char *num = strchr(s, ':');
            if (num != NULL) {
                num++;
                while (*num == ' ' || *num == '"') {
                    num++;
                }
                size = strtoull(num, NULL, 10);
            }
        }
        cb(name, size, ctx);
        p = after;
    }
}

typedef struct {
    char best[128];
    uint64_t best_size;
    bool have_512;
} pick_ctx_t;

static void pick_cb(const char *name, uint64_t size, void *ctx)
{
    pick_ctx_t *c = (pick_ctx_t *)ctx;
    size_t nl = strlen(name);
    if (nl < 5 || strcasecmp(name + nl - 4, ".mp4") != 0) {
        return;
    }
    if (size > 900ull * 1000 * 1000) {
        return;                     /* too big for the board to even try */
    }
    bool is512 = (strstr(name, "512kb") != NULL);
    if (is512 && !c->have_512) {
        c->have_512 = true;
        strlcpy(c->best, name, sizeof(c->best));
        c->best_size = size;
        return;
    }
    if (c->best[0] == '\0' || (!c->have_512 && size < c->best_size)) {
        strlcpy(c->best, name, sizeof(c->best));
        c->best_size = size;
    }
}

films_play_err_t films_resolve(const char *ident, char *url, size_t urlsz)
{
    /* tiny resolve cache: re-tapping the same film must not re-hit the
     * (sometimes flaky) archive.org frontend, and after the frontend
     * browned out mid-session the cached node URL still plays */
    typedef struct { char ident[FILM_IDENT_MAX]; char url[224]; } res_cache_t;
    static res_cache_t cache[8];
    static int cache_n;
    for (int i = 0; i < cache_n; i++) {
        if (strcmp(cache[i].ident, ident) == 0) {
            if (i != 0) {
                res_cache_t t = cache[i];
                memmove(&cache[1], &cache[0], sizeof(res_cache_t) * i);
                cache[0] = t;
            }
            strlcpy(url, cache[0].url, urlsz);
            ESP_LOGI(TAG, "resolve %s: cached %s", ident, url);
            return FILMS_PLAY_OK;
        }
    }

    char meta_url[FILM_IDENT_MAX + 40];
    snprintf(meta_url, sizeof(meta_url), "https://archive.org/metadata/%s", ident);

    prog(FILMS_PROG_METADATA, 0, 0);
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = web_get(meta_url, NULL, &body, &len, NULL);
    if (err != ESP_OK || body == NULL) {
        ESP_LOGW(TAG, "metadata %s failed: %s", ident, esp_err_to_name(err));
        return FILMS_ERR_NET;
    }

    pick_ctx_t pick = { .best = "", .best_size = 0, .have_512 = false };
    metadata_scan_files(body, pick_cb, &pick);

    /* the files live on a cluster node named right in the metadata:
     * "server":"ia801504.us.archive.org", "dir":"/29/items/<id>".
     * A DIRECT node URL skips the archive.org/download redirect frontend,
     * which brownouts far more often than the nodes themselves. */
    char server[64] = "", dir[96] = "";
    json_str_after(body, "server", server, sizeof(server));
    json_str_after(body, "dir", dir, sizeof(dir));
    free(body);

    if (pick.best[0] == '\0') {
        return FILMS_ERR_NO_MP4;
    }
    char enc[320];
    web_url_encode(pick.best, enc, sizeof(enc));

    bool have_node = server[0] != '\0' && dir[0] == '/';
    if (have_node &&
        snprintf(url, urlsz, "https://%s%s/%s", server, dir, enc) >= (int)urlsz) {
        have_node = false;      /* truncated - fall through to /download */
    }
    if (!have_node &&
        snprintf(url, urlsz, "https://archive.org/download/%s/%s",
                 ident, enc) >= (int)urlsz) {
        ESP_LOGW(TAG, "%s: play URL does not fit %u bytes", ident,
                 (unsigned)urlsz);
        return FILMS_ERR_NO_MP4;
    }

    for (int i = cache_n - 1; i > 0; i--) {
        cache[i] = cache[i - 1];
    }
    if (cache_n < (int)(sizeof(cache) / sizeof(cache[0]))) {
        cache_n++;
    }
    strlcpy(cache[0].ident, ident, sizeof(cache[0].ident));
    strlcpy(cache[0].url, url, sizeof(cache[0].url));

    ESP_LOGI(TAG, "resolved %s -> %s (%" PRIu64 " bytes, %s)", ident, pick.best,
             pick.best_size, have_node ? "node" : "download");
    return FILMS_PLAY_OK;
}

/* ------------------------------------------------------------------ */
/* MP4 box walking + H.264 profile check                              */
/* ------------------------------------------------------------------ */

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t be16_at(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static inline uint32_t box_type(const uint8_t *p)
{
    return be32(p + 4);
}

/* Short loggable form: skip scheme, keep host + last 44 chars of path */
static const char *url_short(const char *url)
{
    static char shortbuf[64];
    const char *p = strstr(url, "://");
    p = (p != NULL) ? p + 3 : url;
    size_t len = strlen(p);
    if (len <= sizeof(shortbuf) - 1) {
        snprintf(shortbuf, sizeof(shortbuf), "%s", p);
    } else {
        snprintf(shortbuf, sizeof(shortbuf), "...%s", p + len - 44);
    }
    return shortbuf;
}

/* last HTTP status seen by the probe (0 = no answer), for the log/UI */
static int s_probe_last_status;

int films_probe_last_status(void)
{
    return s_probe_last_status;
}

/* Range-read len bytes at offset; returns bytes read or -1.
 * *strict_range is set false when the server answered 200 (ignored Range).
 * Every failure is logged with the reason - the serial log must show WHY
 * a film was rejected without a PC attached. */
static int range_read(const char *url, uint64_t offset,
                      uint8_t *buf, size_t len, bool *strict_range)
{
    esp_http_client_config_t cfg = {
        .url = url,
        /* 32 KB: the transport reads at most buffer_size per recv, and a
         * 4 KB window on a ~200 ms RTT path capped the stream at ~20 kB/s
         * (a whole-moov probe download took minutes at that rate). */
        .buffer_size = 32 * 1024,
        .buffer_size_tx = 2048,
        .timeout_ms = 20000,
        .max_redirection_count = 10,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ESP_LOGW(TAG, "range %s: client init failed", url_short(url));
        return -1;
    }
    char rh[40];
    snprintf(rh, sizeof(rh), "bytes=%" PRIu64 "-%" PRIu64,
             offset, offset + len - 1);
    esp_http_client_set_header(c, "Range", rh);
    esp_http_client_set_header(c, "Accept-Encoding", "identity");

    int got = -1;
    esp_err_t open_err = esp_http_client_open(c, 0);
    if (open_err == ESP_OK) {
        (void)esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        s_probe_last_status = code;
        bool usable = (code == 206) || (code == 200 && offset == 0);
        if (strict_range != NULL && code == 200) {
            *strict_range = false;
        }
        if (!usable) {
            ESP_LOGW(TAG, "range %s -> HTTP %d (need 206/200)",
                     url_short(url), code);
        } else {
            got = 0;
            uint32_t reported = 0;
            while ((size_t)got < len) {
                int n = esp_http_client_read(c, (char *)buf + got,
                                             (int)(len - (size_t)got));
                if (n <= 0) {
                    break;
                }
                got += n;
                /* feed the busy card: a 3 MB moov at ~20 kB/s is minutes
                 * of dead silence without periodic progress reports */
                if ((uint32_t)got - reported >= 16 * 1024 ||
                    (size_t)got >= len) {
                    reported = (uint32_t)got;
                    prog(FILMS_PROG_FILE, (uint32_t)got, (uint32_t)len);
                }
            }
            ESP_LOGI(TAG, "range %s -> HTTP %d, %d bytes",
                     url_short(url), code, got);
        }
        esp_http_client_close(c);
    } else {
        s_probe_last_status = 0;
        ESP_LOGW(TAG, "range %s: open failed: %s (DNS/TLS/сеть?)",
                 url_short(url), esp_err_to_name(open_err));
    }
    esp_http_client_cleanup(c);
    return got;
}

/* ------------------------------------------------------------------ */
/* Lazy box walk over the remote file                                 */
/*                                                                    */
/* The old probe downloaded the WHOLE moov (3-5 MB on real films =    */
/* minutes at archive.org node speeds) just to reach avcC a few       */
/* hundred bytes past its start. The walk now reads every box header  */
/* on demand: moov/trak/mdia/minf/stbl children are skipped by size,  */
/* only the descent path (hdlr, stsd, avcC, ctts) is actually fetched. */
/* Reads are served from a sliding 64 KB window; a read outside it    */
/* slides the window with one Range request.                          */
/* ------------------------------------------------------------------ */

static inline uint32_t fourcc(const char *s)
{
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}

typedef struct {
    const char *url;
    uint8_t    *win;        /* window contents                     */
    uint64_t    win_base;   /* file offset of win[0]               */
    size_t      win_len;    /* valid bytes in the window           */
    bool        saw_200;    /* a read got 200 - server ignored Range */
} lazy_reader_t;

/* Copy len bytes at file offset off into dst. Served from the window when
 * possible; otherwise the window slides to off with one Range read. Returns
 * false on a short read (EOF or transport failure). */
static bool lazy_read(lazy_reader_t *r, uint64_t off, void *dst, size_t len)
{
    if (len != 0 && off >= r->win_base &&
        off + len <= r->win_base + r->win_len) {
        memcpy(dst, r->win + (size_t)(off - r->win_base), len);
        return true;
    }
    bool strict = true;
    int got = range_read(r->url, off, r->win, PROBE_WINDOW, &strict);
    if (!strict) {
        r->saw_200 = true;
    }
    if (got < (int)len) {
        return false;
    }
    r->win_base = off;
    r->win_len  = (size_t)got;
    memcpy(dst, r->win, len);
    return true;
}

typedef struct {
    uint64_t size;      /* full box size incl. header             */
    uint32_t type;      /* fourcc, big-endian as stored in file   */
    size_t   hdr;       /* header length (8, or 16 for largesize) */
} box_hdr_t;

/* Read a box header at off. Returns false on a short read or a nonsense
 * header (size 0 "extends to EOF" and size < header length included). */
static bool box_hdr_at(lazy_reader_t *r, uint64_t off, box_hdr_t *b)
{
    uint8_t h[16];
    if (!lazy_read(r, off, h, 8)) {
        return false;
    }
    b->hdr  = 8;
    b->size = be32(h);
    b->type = be32(h + 4);
    if (b->size == 1) {
        if (!lazy_read(r, off + 8, h, 8)) {
            return false;
        }
        b->size = be64(h);
        b->hdr  = 16;
    }
    return b->size >= b->hdr;
}

/* H.264 gate for the h264bsd SW decoder (esp_h264_dec_sw):
 *  - Baseline (66): decodes - status quo, what plays today keeps playing;
 *  - Main (77): decodable when constraint_set1 is set - the stream is then
 *    restricted to Baseline tools (CAVLC, no B-slices). h264bsd itself does
 *    not look at profile_idc (its SPS parser only prints a debug note) and
 *    rejects CABAC in the PPS parse, so constrained-Main streams play.
 *    B-frames are guarded via the ctts box: non-zero composition offsets
 *    exist only when pts != dts (B-frames) - cheap protection against
 *    encoders that set constraint_set1 sloppily;
 *  - everything else (High, Main without constraint_set1): rejected here
 *    with a readable reason instead of failing mid-playback. */
static films_play_err_t profile_verdict(uint8_t profile, uint8_t cflags,
                                        bool has_bframes)
{
    if (profile == 66) {
        return FILMS_PLAY_OK;
    }
    if (profile == 77 && (cflags & 0x40) && !has_bframes) {
        return FILMS_PLAY_OK;
    }
    return FILMS_ERR_HIGH_PROFILE;
}

/* Inspect one stbl: scan its children for ctts (B-frame marker) and stsd,
 * then parse the first sample entry + avcC. Lazy: only the 8-byte ctts
 * head and the stsd/avcC payloads are fetched. */
static films_play_err_t stbl_inspect(lazy_reader_t *r, uint64_t stbl_off,
                                     uint64_t stbl_size, uint16_t *out_w,
                                     uint16_t *out_h)
{
    uint64_t end = stbl_off + stbl_size;
    uint64_t stsd_off = 0;
    uint64_t stsd_size = 0;
    bool has_bframes = false;
    bool have_stsd = false;

    uint64_t it = stbl_off + 8;         /* past the stbl header */
    while (it + 8 <= end) {
        box_hdr_t ch;
        if (!box_hdr_at(r, it, &ch) || it + ch.size > end) {
            break;
        }
        if (ch.type == fourcc("ctts") && ch.size >= ch.hdr + 8) {
            /* version/flags(4) + entry_count(4); the offset table is NOT
             * fetched - entry_count > 0 already implies reordered pts */
            uint8_t ct[8];
            if (lazy_read(r, it + ch.hdr, ct, sizeof(ct))) {
                has_bframes = (be32(ct + 4) > 0);
            }
        } else if (ch.type == fourcc("stsd")) {
            stsd_off = it;
            stsd_size = ch.size;
            have_stsd = true;
        }
        it += ch.size;
    }
    if (!have_stsd) {
        return FILMS_ERR_NO_VIDEO;
    }

    /* stsd: hdr(8) + version/flags(4) + entry_count(4) + entries */
    uint8_t h[8];
    if (!lazy_read(r, stsd_off + 8, h, sizeof(h))) {
        return FILMS_ERR_NO_VIDEO;
    }
    uint64_t entry = stsd_off + 16;     /* first sample entry */
    if (!lazy_read(r, entry, h, sizeof(h))) {
        return FILMS_ERR_NO_VIDEO;
    }
    uint64_t esz = be32(h);
    uint32_t etype = be32(h + 4);
    if (esz < 24 || entry + esz > stsd_off + stsd_size) {
        return FILMS_ERR_NO_VIDEO;
    }

    if (etype == fourcc("hvc1") || etype == fourcc("hev1")) {
        return FILMS_ERR_NOT_H264;      /* HEVC */
    }
    if (etype != fourcc("avc1") && etype != fourcc("avc2") &&
        etype != fourcc("avc3") && etype != fourcc("avc4")) {
        return FILMS_ERR_NOT_H264;      /* mpeg4-asp / unknown */
    }

    /* visual sample entry: width/height sit at payload + 24/26 */
    uint8_t vh[28];
    if (lazy_read(r, entry + 8, vh, sizeof(vh))) {
        *out_w = be16_at(vh + 24);
        *out_h = be16_at(vh + 26);
    }

    /* avcC sits among the child boxes after the 78-byte sample entry head */
    uint64_t cend = entry + esz;
    uint64_t cit  = entry + 8 + 78;
    while (cit + 8 <= cend) {
        box_hdr_t ch;
        if (!box_hdr_at(r, cit, &ch) || ch.size < ch.hdr ||
            cit + ch.size > cend) {
            break;
        }
        if (ch.type == fourcc("avcC") && ch.size >= ch.hdr + 4) {
            uint8_t a[4];               /* ver, profile, cflags, level */
            if (!lazy_read(r, cit + ch.hdr, a, sizeof(a))) {
                return FILMS_ERR_NO_VIDEO;
            }
            ESP_LOGI(TAG, "avcC: profile=%u constraint=0x%02x level=%u",
                     a[1], a[2], a[3]);
            return profile_verdict(a[1], a[2], has_bframes);
        }
        cit += ch.size;
    }
    /* avc1 without avcC (or avc3 with in-band SPS/PPS): profile unknown -
     * fail safe */
    return FILMS_ERR_HIGH_PROFILE;
}

/* Walk one trak: mdia -> hdlr (vide?) -> minf -> stbl. Boxes off the path
 * are skipped by their header size alone - a multi-MB audio-trak index
 * costs nothing to step over. */
static films_play_err_t trak_inspect(lazy_reader_t *r, uint64_t trak_off,
                                     uint64_t trak_size, uint16_t *out_w,
                                     uint16_t *out_h)
{
    uint64_t end = trak_off + trak_size;

    uint64_t mdia_off = 0;
    uint64_t mdia_size = 0;
    bool have_mdia = false;
    uint64_t it = trak_off + 8;
    while (it + 8 <= end) {
        box_hdr_t ch;
        if (!box_hdr_at(r, it, &ch) || it + ch.size > end) {
            break;
        }
        if (ch.type == fourcc("mdia")) {
            mdia_off = it;
            mdia_size = ch.size;
            have_mdia = true;
        }
        it += ch.size;
    }
    if (!have_mdia) {
        return FILMS_ERR_NO_VIDEO;      /* caller tries other traks */
    }

    uint64_t mend = mdia_off + mdia_size;
    uint64_t minf_off = 0;
    uint64_t minf_size = 0;
    bool video = false;
    bool have_minf = false;
    it = mdia_off + 8;
    while (it + 8 <= mend) {
        box_hdr_t ch;
        if (!box_hdr_at(r, it, &ch) || it + ch.size > mend) {
            break;
        }
        if (ch.type == fourcc("hdlr") && ch.size >= ch.hdr + 12) {
            /* hdr(8) + version/flags(4) + pre_defined(4) -> handler_type
             * sits at box_start + 16 */
            uint8_t ht[4];
            if (lazy_read(r, it + 16, ht, sizeof(ht))) {
                video = (be32(ht) == fourcc("vide"));
            }
        } else if (ch.type == fourcc("minf")) {
            minf_off = it;
            minf_size = ch.size;
            have_minf = true;
        }
        it += ch.size;
    }
    if (!video || !have_minf) {
        return FILMS_ERR_NO_VIDEO;      /* audio / hint trak, or no minf */
    }

    uint64_t fend = minf_off + minf_size;
    it = minf_off + 8;
    while (it + 8 <= fend) {
        box_hdr_t ch;
        if (!box_hdr_at(r, it, &ch) || it + ch.size > fend) {
            break;
        }
        if (ch.type == fourcc("stbl")) {
            return stbl_inspect(r, it, ch.size, out_w, out_h);
        }
        it += ch.size;
    }
    return FILMS_ERR_NO_VIDEO;
}

/* Walk the moov: try every trak, return the first real codec verdict
 * (OK / HIGH_PROFILE / NOT_H264). NO_VIDEO = no video trak carried a
 * usable sample entry - same semantics as the old whole-buffer walk. */
static films_play_err_t moov_inspect(lazy_reader_t *r, uint64_t moov_off,
                                     uint16_t *out_w, uint16_t *out_h)
{
    box_hdr_t m;
    if (!box_hdr_at(r, moov_off, &m) || m.type != fourcc("moov")) {
        return FILMS_ERR_NO_MP4;
    }
    uint64_t end = moov_off + m.size;
    uint64_t it = moov_off + m.hdr;
    while (it + 8 <= end) {
        box_hdr_t ch;
        if (!box_hdr_at(r, it, &ch) || it + ch.size > end) {
            break;
        }
        if (ch.type == fourcc("trak")) {
            films_play_err_t vr = trak_inspect(r, it, ch.size, out_w, out_h);
            if (vr == FILMS_PLAY_OK || vr == FILMS_ERR_HIGH_PROFILE ||
                vr == FILMS_ERR_NOT_H264) {
                return vr;              /* real codec verdict */
            }
            /* this trak had no usable stsd: keep trying other traks */
        }
        it += ch.size;
    }
    return FILMS_ERR_NO_VIDEO;
}

/* Locate the moov box and walk it lazily. Three layouts handled:
 *  - moov inside the first window (faststart): served from it, no extra
 *    Range requests at all;
 *  - moov spilling past the first window: the lazy walk slides the window
 *    on demand;
 *  - moov after mdat: hop box headers with tiny Range reads (unchanged)
 *    until the moov header shows up, then walk it.
 * Whatever the layout, only a few dozen KB ever leave the server - the old
 * "download the whole moov" path (up to 12 MB, minutes per probe) is gone. */
static films_play_err_t mp4_walk(const char *url, uint16_t *out_w,
                                 uint16_t *out_h)
{
    uint8_t *chunk = heap_caps_malloc(PROBE_WINDOW, MALLOC_CAP_SPIRAM);
    if (chunk == NULL) {
        return FILMS_ERR_NET;
    }

    films_play_err_t verdict = FILMS_ERR_NET;
    bool strict = true;
    int got = range_read(url, 0, chunk, PROBE_WINDOW, &strict);
    if (got < 16) {
        heap_caps_free(chunk);
        return FILMS_ERR_NET;
    }

    /* top-level walk on the first window */
    size_t off = 0;
    int64_t moov_off = -1;
    uint64_t moov_size = 0;
    for (int guard = 0; guard < 16 && off + 8 <= (size_t)got; guard++) {
        uint64_t sz = be32(chunk + off);
        size_t hdr = 8;
        if (sz == 1) {
            if (off + 16 > (size_t)got) {
                break;
            }
            sz = be64(chunk + off + 8);
            hdr = 16;
        }
        if (sz < hdr) {
            break;
        }
        if (box_type(chunk + off) == fourcc("ftyp") && off != 0) {
            break;      /* not an MP4 layout we understand */
        }
        if (box_type(chunk + off) == fourcc("moov")) {
            moov_off = (int64_t)off;
            moov_size = sz;
            break;
        }
        if (box_type(chunk + off) == fourcc("mdat")) {
            break;      /* moov is after mdat: find it below */
        }
        off += sz;
    }

    do {
        if (moov_off < 0 && !strict) {
            /* the head read came back 200 - this server ignores Range, so
             * any further offset read would return data from position 0 */
            verdict = FILMS_ERR_NO_RANGE;
            break;
        }
        if (moov_off < 0) {
            /* mdat came first: box-walk headers with tiny range reads */
            uint8_t hdr[16];
            uint64_t box_pos = (uint64_t)off;   /* offset of the mdat header */
            for (int guard = 0; guard < 16; guard++) {
                int h = range_read(url, box_pos, hdr, sizeof(hdr), &strict);
                if (h < 8) {
                    break;
                }
                uint64_t sz = be32(hdr);
                size_t hsz = 8;
                if (sz == 1) {
                    if (h < 16) {
                        break;      /* short read across a largesize header */
                    }
                    sz = be64(hdr + 8);
                    hsz = 16;
                }
                if (sz < hsz) {
                    break;
                }
                if (box_type(hdr) == fourcc("moov")) {
                    moov_off = (int64_t)box_pos;
                    moov_size = sz;
                    break;
                }
                box_pos += sz;
            }
            if (moov_off < 0) {
                verdict = FILMS_ERR_NO_MP4;
                break;
            }
        }
        if (moov_size > MOOV_SANITY_MAX) {
            ESP_LOGW(TAG, "moov index %llu bytes > %u MB sanity limit",
                     (unsigned long long)moov_size,
                     (unsigned)(MOOV_SANITY_MAX >> 20));
            verdict = FILMS_ERR_BIG_MOOV;
            break;
        }
        lazy_reader_t reader = {
            .url      = url,
            .win      = chunk,
            .win_base = 0,
            .win_len  = (size_t)got,
        };
        verdict = moov_inspect(&reader, (uint64_t)moov_off, out_w, out_h);
        if (verdict == FILMS_ERR_NET && reader.saw_200) {
            verdict = FILMS_ERR_NO_RANGE;
        }
    } while (0);

    heap_caps_free(chunk);
    ESP_LOGI(TAG, "probe %s -> %s %ux%u", url, films_err_str(verdict),
             *out_w, *out_h);
    return verdict;
}

/* Verdict cache keyed by the play URL. Re-tapping a film must not repeat
 * the probe: a rejected film then fails instantly (the busy card barely
 * flashes) and an accepted one skips straight to the player prep - the
 * user's log showed the same 4.5 MB moov walked twice for one film because
 * the second tap re-probed. Only transport-independent verdicts are
 * cached; NET / NO_RANGE stay out so a flaky node is retried on the next
 * tap. */
typedef struct {
    char             url[FILMS_URL_MAX];
    films_play_err_t verdict;
    uint16_t         w, h;
} probe_cache_t;
static probe_cache_t s_probe_cache[8];
static int           s_probe_cache_n;

films_play_err_t films_probe_url(const char *url, uint16_t *out_w,
                                 uint16_t *out_h)
{
    *out_w = 0;
    *out_h = 0;
    if (url == NULL || strncasecmp(url, "http", 4) != 0) {
        return FILMS_PLAY_OK;   /* local file: nothing to check */
    }

    for (int i = 0; i < s_probe_cache_n; i++) {
        if (strcmp(s_probe_cache[i].url, url) == 0) {
            if (i != 0) {
                probe_cache_t t = s_probe_cache[i];
                memmove(&s_probe_cache[1], &s_probe_cache[0],
                        sizeof(probe_cache_t) * (size_t)i);
                s_probe_cache[0] = t;
            }
            ESP_LOGI(TAG, "probe %s: cached %ux%u %s", url,
                     s_probe_cache[0].w, s_probe_cache[0].h,
                     films_err_str(s_probe_cache[0].verdict));
            *out_w = s_probe_cache[0].w;
            *out_h = s_probe_cache[0].h;
            return s_probe_cache[0].verdict;
        }
    }

    films_play_err_t r = mp4_walk(url, out_w, out_h);
    if (r == FILMS_ERR_NET) {
        /* archive.org nodes occasionally drop the follow-up connection
         * outright (instant TLS failure right after a good 64 KB read).
         * One clean retry turns most of those false rejects into plays;
         * genuine profile/codec verdicts are NOT retried. */
        ESP_LOGW(TAG, "probe: transient net failure, retrying once");
        vTaskDelay(pdMS_TO_TICKS(800));
        r = mp4_walk(url, out_w, out_h);
    }

    if (r == FILMS_PLAY_OK || r == FILMS_ERR_BIG_MOOV ||
        r == FILMS_ERR_NOT_H264 || r == FILMS_ERR_HIGH_PROFILE ||
        r == FILMS_ERR_NO_MP4 || r == FILMS_ERR_NO_VIDEO) {
        probe_cache_t e = { .verdict = r, .w = *out_w, .h = *out_h };
        strlcpy(e.url, url, sizeof(e.url));
        for (int i = s_probe_cache_n - 1; i > 0; i--) {
            s_probe_cache[i] = s_probe_cache[i - 1];
        }
        if (s_probe_cache_n <
            (int)(sizeof(s_probe_cache) / sizeof(s_probe_cache[0]))) {
            s_probe_cache_n++;
        }
        s_probe_cache[0] = e;
    }
    return r;
}

/* ------------------------------------------------------------------ */

const char *films_err_str(films_play_err_t e)
{
    switch (e) {
    case FILMS_PLAY_OK:       return "ok";
    case FILMS_ERR_NET:       return "сервер не ответил";
    case FILMS_ERR_BIG_MOOV:  return "индекс файла слишком велик";
    case FILMS_ERR_NO_RESULT: return "ничего не найдено";
    case FILMS_ERR_NO_MP4:    return "нет mp4-версии";
    case FILMS_ERR_NO_RANGE:  return "сервер без докачки";
    case FILMS_ERR_NOT_H264:  return "кодек не H.264";
    case FILMS_ERR_HIGH_PROFILE:
        return "нужен H.264 Baseline/Main(constr.)";
    case FILMS_ERR_NO_VIDEO:  return "нет видеодорожки";
    default:                  return "ошибка";
    }
}

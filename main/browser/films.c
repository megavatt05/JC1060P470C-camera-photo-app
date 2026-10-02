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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "browser/web_client.h"
#include "browser/films.h"

static const char *TAG = "films";

#define MOOV_MAX        (4u << 20)  /* 4 MB is plenty even for a 2-hour film  */
#define FIRST_CHUNK     (64 * 1024)

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
    char meta_url[FILM_IDENT_MAX + 40];
    snprintf(meta_url, sizeof(meta_url), "https://archive.org/metadata/%s", ident);

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = web_get(meta_url, NULL, &body, &len, NULL);
    if (err != ESP_OK || body == NULL) {
        ESP_LOGW(TAG, "metadata %s failed: %s", ident, esp_err_to_name(err));
        return FILMS_ERR_NET;
    }

    pick_ctx_t pick = { .best = "", .best_size = 0, .have_512 = false };
    metadata_scan_files(body, pick_cb, &pick);
    free(body);

    if (pick.best[0] == '\0') {
        return FILMS_ERR_NO_MP4;
    }
    char enc[160];
    web_url_encode(pick.best, enc, sizeof(enc));
    snprintf(url, urlsz, "https://archive.org/download/%s/%s", ident, enc);
    ESP_LOGI(TAG, "resolved %s -> %s (%" PRIu64 " bytes)", ident, pick.best,
             pick.best_size);
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

/* Range-read len bytes at offset; returns bytes read or -1.
 * *strict_range is set false when the server answered 200 (ignored Range). */
static int range_read(const char *url, uint64_t offset,
                      uint8_t *buf, size_t len, bool *strict_range)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .timeout_ms = 12000,
        .max_redirection_count = 10,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return -1;
    }
    char rh[40];
    snprintf(rh, sizeof(rh), "bytes=%" PRIu64 "-%" PRIu64,
             offset, offset + len - 1);
    esp_http_client_set_header(c, "Range", rh);
    esp_http_client_set_header(c, "Accept-Encoding", "identity");

    int got = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int status = esp_http_client_fetch_headers(c);
        (void)status;
        int code = esp_http_client_get_status_code(c);
        bool usable = (code == 206) || (code == 200 && offset == 0);
        if (strict_range != NULL && code == 200) {
            *strict_range = false;
        }
        if (usable) {
            got = 0;
            while ((size_t)got < len) {
                int n = esp_http_client_read(c, (char *)buf + got,
                                             (int)(len - (size_t)got));
                if (n <= 0) {
                    break;
                }
                got += n;
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return got;
}

/* find a child box by type in [buf, buf+size); buf must point INSIDE the
 * container (past its own 8-byte header); returns offset relative to buf */
static int64_t find_box(const uint8_t *buf, size_t size, uint32_t type)
{
    size_t off = 0;
    while (off + 8 <= size) {
        uint64_t sz = be32(buf + off);
        size_t hdr = 8;
        if (sz == 1) {
            if (off + 16 > size) {
                break;
            }
            sz = be64(buf + off + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = size - off;
        }
        if (sz < hdr || off + sz > size) {
            break;
        }
        if (box_type(buf + off) == type) {
            return (int64_t)off;
        }
        off += sz;
    }
    return -1;
}

/* inspect stbl -> stsd of one video trak */
static films_play_err_t stsd_inspect(const uint8_t *stbl, size_t stbl_size,
                                     uint16_t *out_w, uint16_t *out_h)
{
    /* stbl points at the stbl box header: search inside its body */
    int64_t stsd = find_box(stbl + 8, stbl_size - 8, be32((const uint8_t *)"stsd"));
    if (stsd < 0) {
        return FILMS_ERR_NO_VIDEO;
    }
    const uint8_t *s = stbl + 8 + stsd;
    size_t sz = be32(s);
    if (sz < 24 || sz > stbl_size - 8 - stsd) {
        return FILMS_ERR_NO_VIDEO;
    }
    const uint8_t *entry = s + 16;  /* version/flags(4) + entry_count(4) */
    size_t esz = be32(entry);
    if (esz < 24 || entry + esz > s + sz) {
        return FILMS_ERR_NO_VIDEO;
    }
    uint32_t type = box_type(entry);
    const uint8_t *p = entry + 8;
    size_t plen = esz - 8;

    if (type == be32((const uint8_t *)"hvc1") ||
        type == be32((const uint8_t *)"hev1")) {
        return FILMS_ERR_NOT_H264;      /* HEVC */
    }
    if (type != be32((const uint8_t *)"avc1") &&
        type != be32((const uint8_t *)"avc2") &&
        type != be32((const uint8_t *)"avc3") &&
        type != be32((const uint8_t *)"avc4")) {
        return FILMS_ERR_NOT_H264;      /* mpeg4-asp / unknown */
    }

    if (plen >= 28) {
        *out_w = be16_at(p + 24);
        *out_h = be16_at(p + 26);
    }

    /* avcC sits among the child boxes after the 78-byte sample entry head */
    const uint8_t *end = p + plen;
    const uint8_t *q = (plen > 78) ? p + 78 : end;
    while (q + 8 <= end) {
        uint32_t csz = be32(q);
        if (csz < 8 || q + csz > end) {
            break;
        }
        if (box_type(q) == be32((const uint8_t *)"avcC") && csz >= 12) {
            uint8_t profile = q[8 + 1];     /* payload: ver, profile, cflags, level */
            ESP_LOGI(TAG, "avcC: profile=%u constraint=0x%02x level=%u",
                     profile, q[8 + 2], q[8 + 3]);
            return (profile == 66) ? FILMS_PLAY_OK : FILMS_ERR_HIGH_PROFILE;
        }
        q += csz;
    }
    /* avc1 without avcC (or avc3 with in-band SPS/PPS): profile unknown,
     * the SW decoder rejects anything but 66 - fail safe */
    return FILMS_ERR_HIGH_PROFILE;
}

static films_play_err_t moov_inspect(const uint8_t *moov, size_t size,
                                     uint16_t *out_w, uint16_t *out_h)
{
    size_t off = 8;                 /* skip the moov header itself */
    while (off + 8 <= size) {
        uint64_t sz = be32(moov + off);
        if (sz < 8 || off + sz > size) {
            break;
        }
        if (box_type(moov + off) == be32((const uint8_t *)"trak")) {
            const uint8_t *trak_body = moov + off + 8;
            size_t trak_body_sz = (size_t)sz - 8;
            int64_t mdia = find_box(trak_body, trak_body_sz,
                                    be32((const uint8_t *)"mdia"));
            if (mdia >= 0 && trak_body_sz >= (size_t)mdia + 8) {
                const uint8_t *mdia_box = trak_body + mdia;
                size_t mdia_sz = be32(mdia_box);
                if (mdia_sz >= 16) {
                    const uint8_t *mdia_body = mdia_box + 8;
                    size_t mdia_body_sz = mdia_sz - 8;
                    int64_t hdlr = find_box(mdia_body, mdia_body_sz,
                                            be32((const uint8_t *)"hdlr"));
                    bool video = false;
                    if (hdlr >= 0 && mdia_body_sz >= (size_t)hdlr + 20) {
                        /* hdlr box: hdr(8) + version/flags(4) + pre_defined(4)
                         * -> handler_type sits at box_start + 16 */
                        video = (be32(mdia_body + hdlr + 16) ==
                                 be32((const uint8_t *)"vide"));
                    }
                    if (video) {
                        int64_t minf = find_box(mdia_body, mdia_body_sz,
                                                be32((const uint8_t *)"minf"));
                        if (minf >= 0) {
                            const uint8_t *minf_box = mdia_body + minf;
                            size_t minf_sz = be32(minf_box);
                            if (minf_sz >= 16) {
                                const uint8_t *minf_body = minf_box + 8;
                                size_t minf_body_sz = minf_sz - 8;
                                int64_t stbl = find_box(minf_body, minf_body_sz,
                                                        be32((const uint8_t *)"stbl"));
                                if (stbl >= 0) {
                                    const uint8_t *stbl_box = minf_body + stbl;
                                    films_play_err_t r = stsd_inspect(
                                        stbl_box, be32(stbl_box), out_w, out_h);
                                    if (r == FILMS_PLAY_OK ||
                                        r == FILMS_ERR_HIGH_PROFILE ||
                                        r == FILMS_ERR_NOT_H264) {
                                        return r;   /* real codec verdict */
                                    }
                                    /* this video trak had no usable stsd: keep
                                     * it as a fallback but try other traks */
                                }
                            }
                        }
                    }
                }
            }
        }
        off += sz;
    }
    return FILMS_ERR_NO_VIDEO;
}

static films_play_err_t mp4_walk(const char *url, uint16_t *out_w, uint16_t *out_h)
{
    uint8_t *chunk = heap_caps_malloc(FIRST_CHUNK, MALLOC_CAP_SPIRAM);
    if (chunk == NULL) {
        return FILMS_ERR_NET;
    }

    films_play_err_t verdict = FILMS_ERR_NET;
    bool strict = true;
    int got = range_read(url, 0, chunk, FIRST_CHUNK, &strict);
    if (got < 16) {
        heap_caps_free(chunk);
        return FILMS_ERR_NET;
    }

    /* top-level walk on the first chunk */
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
        if (box_type(chunk + off) == be32((const uint8_t *)"ftyp") && off != 0) {
            break;      /* not an MP4 layout we understand */
        }
        if (box_type(chunk + off) == be32((const uint8_t *)"moov")) {
            moov_off = (int64_t)off;
            moov_size = sz;
            break;
        }
        if (box_type(chunk + off) == be32((const uint8_t *)"mdat")) {
            break;      /* moov is after mdat: fetch it below */
        }
        off += sz;
    }

    do {
        if (moov_off >= 0 &&
            (uint64_t)moov_off + moov_size <= (uint64_t)got) {
            verdict = moov_inspect(chunk + moov_off, (size_t)moov_size,
                                   out_w, out_h);
            break;
        }
        if (moov_off >= 0) {
            /* moov starts inside the first chunk but spills past it */
            if (moov_size > MOOV_MAX) {
                verdict = FILMS_ERR_NET;
                break;
            }
            uint8_t *moov = heap_caps_malloc((size_t)moov_size, MALLOC_CAP_SPIRAM);
            if (moov == NULL) {
                verdict = FILMS_ERR_NET;
                break;
            }
            memcpy(moov, chunk + moov_off, (size_t)got - (size_t)moov_off);
            int tail = range_read(url, (uint64_t)got,
                                  moov + got - moov_off,
                                  (size_t)moov_size - ((size_t)got - (size_t)moov_off),
                                  &strict);
            if (tail >= 0) {
                verdict = moov_inspect(moov, (size_t)moov_size, out_w, out_h);
            } else {
                verdict = strict ? FILMS_ERR_NET : FILMS_ERR_NO_RANGE;
            }
            heap_caps_free(moov);
            break;
        }
        if (!strict) {
            verdict = FILMS_ERR_NO_RANGE;
            break;
        }
        /* mdat came first: box-walk headers with tiny range reads */
        uint8_t hdr[16];
        uint64_t box_pos = (uint64_t)off;   /* offset of the mdat header */
        int64_t m_moov = -1;
        uint64_t m_size = 0;
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
            if (box_type(hdr) == be32((const uint8_t *)"moov")) {
                m_moov = (int64_t)box_pos;
                m_size = sz;
                break;
            }
            box_pos += sz;
        }
        if (m_moov < 0 || m_size > MOOV_MAX) {
            verdict = FILMS_ERR_NO_MP4;
            break;
        }
        uint8_t *moov = heap_caps_malloc((size_t)m_size, MALLOC_CAP_SPIRAM);
        if (moov == NULL) {
            verdict = FILMS_ERR_NET;
            break;
        }
        int t = range_read(url, (uint64_t)m_moov, moov, (size_t)m_size, &strict);
        if (t == (int)m_size) {
            verdict = moov_inspect(moov, (size_t)m_size, out_w, out_h);
        } else {
            verdict = FILMS_ERR_NET;
        }
        heap_caps_free(moov);
    } while (0);

    heap_caps_free(chunk);
    ESP_LOGI(TAG, "probe %s -> %s %ux%u", url, films_err_str(verdict),
             *out_w, *out_h);
    return verdict;
}

films_play_err_t films_probe_url(const char *url, uint16_t *out_w, uint16_t *out_h)
{
    *out_w = 0;
    *out_h = 0;
    if (url == NULL || strncasecmp(url, "http", 4) != 0) {
        return FILMS_PLAY_OK;   /* local file: nothing to check */
    }
    return mp4_walk(url, out_w, out_h);
}

/* ------------------------------------------------------------------ */

const char *films_err_str(films_play_err_t e)
{
    switch (e) {
    case FILMS_PLAY_OK:       return "ok";
    case FILMS_ERR_NET:       return "сеть недоступна/таймаут";
    case FILMS_ERR_NO_RESULT: return "ничего не найдено";
    case FILMS_ERR_NO_MP4:    return "у фильма нет mp4-версии";
    case FILMS_ERR_NO_RANGE:  return "сервер не поддерживает докачку";
    case FILMS_ERR_NOT_H264:  return "кодек не H.264 (HEVC/MPEG4)";
    case FILMS_ERR_HIGH_PROFILE:
        return "профиль H.264 Main/High не поддерживается (нужен Baseline)";
    case FILMS_ERR_NO_VIDEO:  return "в файле нет видеодорожки";
    default:                  return "ошибка";
    }
}

/* Functional host test for main/browser/films.c (the mp4 probe).
 *
 * Builds synthetic MP4 files in memory, serves them through the
 * esp_http_client stubs (Range honored, request/byte counters attached)
 * and asserts probe verdicts, request counts and the verdict cache.
 *
 * Build (see run_tests.sh):
 *   gcc -Wall -Wextra -Iscripts/uihost/stubs -Imain \
 *       scripts/uihost/test_films_host.c main/browser/films.c -o test_films_host
 *
 * The stubs below implement what films.c calls: init/set_header/open/
 * fetch_headers/status/read/close/cleanup. No TLS, no sockets.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "browser/films.h"

/* ------------------------------------------------------------------ */
/* Fake remote file + http stub                                        */
/* ------------------------------------------------------------------ */

static uint8_t  g_file[8u << 20];
static size_t   g_file_len;
static int      g_requests;             /* open() calls               */
static size_t   g_bytes;                /* bytes actually served      */
static int      g_fail_open_at = -1;    /* fail this request index    */
static bool     g_no_range;             /* answer 200, ignore Range   */
static char     g_range_hdr[64];

typedef struct {
    bool     open;
    bool     have_range;
    uint64_t from, to;
    uint64_t served;                    /* bytes already delivered    */
} http_conn_t;

static http_conn_t g_conn;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    (void)cfg;
    g_range_hdr[0] = '\0';
    return (esp_http_client_handle_t)&g_conn;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,
                                     const char *k, const char *v)
{
    (void)c;
    if (strcmp(k, "Range") == 0) {
        snprintf(g_range_hdr, sizeof(g_range_hdr), "%s", v);
    }
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)write_len;
    http_conn_t *conn = (http_conn_t *)c;
    if (g_fail_open_at == g_requests) {
        g_requests++;                   /* transport failure, no conn */
        return ESP_FAIL;
    }
    memset(conn, 0, sizeof(*conn));
    uint64_t a = 0, b = 0;
    if (sscanf(g_range_hdr, "bytes=%llu-%llu",
               (unsigned long long *)&a, (unsigned long long *)&b) == 2) {
        conn->from = a;
        conn->to = b;
        conn->have_range = true;
    }
    conn->open = true;
    g_requests++;
    return ESP_OK;
}

int esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    (void)c;
    return 200;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    (void)c;
    return (g_no_range || !((http_conn_t *)c)->have_range) ? 200 : 206;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    http_conn_t *conn = (http_conn_t *)c;
    if (!conn->open || len <= 0) {
        return 0;
    }
    uint64_t start = (g_no_range || !conn->have_range) ? 0 : conn->from;
    uint64_t end   = (g_no_range || !conn->have_range)
                     ? (uint64_t)g_file_len - 1 : conn->to;
    if (start >= g_file_len) {
        return 0;
    }
    if (end >= g_file_len) {
        end = g_file_len - 1;
    }
    uint64_t left = (end + 1 - start) - conn->served;
    if (left == 0) {
        return 0;
    }
    size_t n = ((uint64_t)len < left) ? (size_t)len : (size_t)left;
    memcpy(buf, g_file + start + conn->served, n);
    conn->served += n;
    g_bytes += n;
    return (int)n;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    ((http_conn_t *)c)->open = false;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    (void)c;
    return ESP_OK;
}

void esp_crt_bundle_attach(void *conf)
{
    (void)conf;
}

int vTaskDelay(int ticks)
{
    (void)ticks;
    return 0;
}

/* web_client stubs - films.c uses them only for search/resolve, which the
 * probe tests never touch */
esp_err_t web_get(const char *url, const char *extra_cookie,
                  char **out_body, size_t *out_len, int *out_status)
{
    (void)url; (void)extra_cookie; (void)out_status;
    *out_body = NULL;
    *out_len = 0;
    return ESP_FAIL;
}

size_t web_url_encode(const char *src, char *dst, size_t dst_size)
{
    (void)src;
    if (dst_size > 0) {
        dst[0] = '\0';
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MP4 builder: sequential box writer over g_file                      */
/* ------------------------------------------------------------------ */

static size_t g_pos;                    /* builder cursor             */

static void wr(const void *p, size_t n)
{
    memcpy(g_file + g_pos, p, n);
    g_pos += n;
    if (g_pos > g_file_len) {
        g_file_len = g_pos;
    }
}

static void wr_u32(uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    wr(b, 4);
}

static void wr_u64(uint64_t v)
{
    wr_u32((uint32_t)(v >> 32));
    wr_u32((uint32_t)v);
}

static size_t begin_box(const char *type)
{
    size_t at = g_pos;
    wr_u32(0);
    wr(type, 4);
    return at;
}

static size_t begin_box64(const char *type)     /* largesize variant */
{
    size_t at = g_pos;
    wr_u32(1);
    wr(type, 4);
    wr_u64(0);
    return at;
}

static void end_box(size_t hdr)
{
    uint64_t sz = (uint64_t)g_pos - hdr;
    uint32_t m = ((uint32_t)g_file[hdr] << 24) |
                 ((uint32_t)g_file[hdr + 1] << 16) |
                 ((uint32_t)g_file[hdr + 2] << 8) | g_file[hdr + 3];
    if (m == 1) {
        uint8_t b[8];
        for (int i = 0; i < 8; i++) {
            b[i] = (uint8_t)(sz >> (56 - 8 * i));
        }
        memcpy(g_file + hdr + 8, b, 8);
    } else {
        uint8_t b[4] = { (uint8_t)(sz >> 24), (uint8_t)(sz >> 16),
                         (uint8_t)(sz >> 8), (uint8_t)sz };
        memcpy(g_file + hdr, b, 4);
    }
}

static void add_u32(uint32_t v) { wr_u32(v); }
static void add_u16(uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    wr(b, 2);
}
static void add_zeros(size_t n) { memset(g_file + g_pos, 0, n); g_pos += n; if (g_pos > g_file_len) g_file_len = g_pos; }

/* hdlr payload: ver/flags + pre_defined + handler_type */
static void add_hdlr(const char *handler)
{
    add_u32(0);
    add_u32(0);
    wr(handler, 4);
}

/* stbl children */
static void add_stts(void)              /* empty table */
{
    size_t b = begin_box("stts");
    add_u32(0); add_u32(0);
    end_box(b);
}

static void add_ctts(uint32_t entries, uint32_t offset) /* B-frames marker */
{
    size_t b = begin_box("ctts");
    add_u32(0);
    add_u32(entries);
    for (uint32_t i = 0; i < entries; i++) {
        add_u32(1);                     /* sample_count */
        add_u32(offset);                /* sample_offset: != 0 */
    }
    end_box(b);
}

static void add_stsz_inflated(size_t table_bytes)  /* big dummy table */
{
    size_t b = begin_box("stsz");
    add_u32(0);
    add_u32(0);                         /* uniform sample size */
    add_u32((uint32_t)(table_bytes / 4));
    while (table_bytes--) {
        g_file[g_pos] = 0x7f;
        g_pos++;
        if (g_pos > g_file_len) {
            g_file_len = g_pos;
        }
    }
    end_box(b);
}

static void add_stco(void)
{
    size_t b = begin_box("stco");
    add_u32(0); add_u32(0);
    end_box(b);
}

/* avc1 sample entry: 78-byte head + avcC child. profile/cflags/level go
 * into the avcC payload (ver, profile, cflags, level). */
static void add_avc1_entry(uint16_t w, uint16_t h, uint8_t profile,
                           uint8_t cflags, uint8_t level)
{
    size_t e = begin_box("avc1");
    add_zeros(6); add_u16(1);           /* reserved + data_ref_idx  */
    add_zeros(16);                      /* pre_defined/reserved     */
    add_u16(w); add_u16(h);             /* width / height @24/26    */
    add_u32(0x00480000); add_u32(0x00480000);  /* horiz/vert res    */
    add_u32(0);                         /* reserved                 */
    add_u16(1);                         /* frame_count              */
    add_zeros(32);                      /* compressorname           */
    add_u16(0x0018); add_u16(0xffff);   /* depth + pre_defined      */
    /* (78 bytes of head so far) */
    size_t c = begin_box("avcC");
    uint8_t head[4] = { 1, profile, cflags, level };   /* ver, profile,
                                                          compat, level */
    wr(head, 4);
    add_zeros(15);                      /* rest of the avcC payload */
    end_box(c);
    end_box(e);
}

static void add_mp4v_entry(uint16_t w, uint16_t h)
{
    size_t e = begin_box("mp4v");
    add_zeros(6); add_u16(1);
    add_zeros(16);
    add_u16(w); add_u16(h);
    add_zeros(46);                      /* rest of the 78-byte head */
    size_t c = begin_box("esds");
    add_u32(0); add_zeros(8);
    end_box(c);
    end_box(e);
}

static void add_stsd(const char *entry_type, uint16_t w, uint16_t h,
                     uint8_t profile, uint8_t cflags, uint8_t level)
{
    size_t b = begin_box("stsd");
    add_u32(0); add_u32(1);             /* version/flags + count    */
    if (strcmp(entry_type, "avc1") == 0) {
        add_avc1_entry(w, h, profile, cflags, level);
    } else if (strcmp(entry_type, "mp4v") == 0) {
        add_mp4v_entry(w, h);
    } else {
        size_t e = begin_box(entry_type);
        add_zeros(78);
        end_box(e);
    }
    end_box(b);
}

/* Video trak: tkhd + mdia{mdhd, hdlr(vide), minf{vmhd, stbl{...}}} */
static void add_trak_video(uint16_t w, uint16_t h, uint8_t profile,
                           uint8_t cflags, bool with_ctts,
                           size_t inflate_stsz, bool largesize_trak)
{
    size_t t = largesize_trak ? begin_box64("trak") : begin_box("trak");
    size_t tk = begin_box("tkhd"); add_zeros(8); end_box(tk);
    size_t md = begin_box("mdia");
    size_t mh = begin_box("mdhd"); add_zeros(8); end_box(mh);
    size_t hl = begin_box("hdlr"); add_hdlr("vide"); end_box(hl);
    size_t mi = begin_box("minf");
    size_t vm = begin_box("vmhd"); add_zeros(4); end_box(vm);
    size_t st = begin_box("stbl");
    add_stsd("avc1", w, h, profile, cflags, 30);
    add_stts();
    if (with_ctts) {
        add_ctts(4, 2);
    }
    if (inflate_stsz > 0) {
        add_stsz_inflated(inflate_stsz);
    }
    add_stco();
    end_box(st);
    end_box(mi);
    end_box(md);
    end_box(t);
}

/* Audio trak with an inflated index - stepping over it must be free */
static void add_trak_audio(size_t inflate_stsz)
{
    size_t t = begin_box("trak");
    size_t tk = begin_box("tkhd"); add_zeros(8); end_box(tk);
    size_t md = begin_box("mdia");
    size_t mh = begin_box("mdhd"); add_zeros(8); end_box(mh);
    size_t hl = begin_box("hdlr"); add_hdlr("soun"); end_box(hl);
    size_t mi = begin_box("minf");
    size_t sm = begin_box("smhd"); add_zeros(4); end_box(sm);
    size_t st = begin_box("stbl");
    add_stsd("mp4a", 0, 0, 0, 0, 0);
    add_stts();
    if (inflate_stsz > 0) {
        add_stsz_inflated(inflate_stsz);
    }
    add_stco();
    end_box(st);
    end_box(mi);
    end_box(md);
    end_box(t);
}

static void add_ftyp(void)
{
    size_t b = begin_box("ftyp");
    wr("isom", 4);
    wr_u32(0x200);
    end_box(b);
}

static void add_mdat(size_t n)
{
    size_t b = begin_box("mdat");
    add_zeros(n);
    end_box(b);
}

/* mvhd - first child of moov in real files */
static void add_mvhd(void)
{
    size_t b = begin_box("mvhd");
    add_zeros(8);
    end_box(b);
}

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                    */
/* ------------------------------------------------------------------ */

static int g_pass, g_fail;

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("FAIL: %s\n", msg); }               \
} while (0)

static films_play_err_t probe(const char *url, uint16_t *w, uint16_t *h)
{
    return films_probe_url(url, w, h);
}

static void reset_http(void)
{
    g_requests = 0;
    g_bytes = 0;
    g_fail_open_at = -1;
    g_no_range = false;
}

/* ------------------------------------------------------------------ */
/* Real-file cases: ffmpeg-produced MP4s (real box layouts - mdhd v1,  */
/* real avcC with SPS/PPS, 'free' boxes) loaded from disk.             */
/* ------------------------------------------------------------------ */

static bool load_real(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("SKIP (no %s)\n", path);
        return false;
    }
    g_file_len = fread(g_file, 1, sizeof(g_file), f);
    fclose(f);
    return true;
}

/* Constraint-flag patching of the real fixtures is done by run_tests.sh
 * (python) before the test runs - see real_main_cs1*.mp4 there. */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    char url[64];
    uint16_t w, h;
    films_play_err_t r;

    /* 1. faststart, Baseline 66: verdict from the first window, exactly
     *    one request */
    snprintf(url, sizeof(url), "https://fake/t01-faststart.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    size_t mv = begin_box("moov");
    add_mvhd();
    add_trak_video(640, 272, 66, 0xC0, false, 0, false);
    add_trak_audio(64);
    end_box(mv);
    add_mdat(1024);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_PLAY_OK, "t01 verdict");
    CHECK(w == 640 && h == 272, "t01 dimensions");
    CHECK(g_requests == 1, "t01 single request");

    /* 2. moov after 1 MB mdat, Main 77 + constraint_set1, no ctts, 3 MB
     *    stsz inside: must pass AND fetch only a few windows */
    snprintf(url, sizeof(url), "https://fake/t02-main-cs1.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    add_mdat(1u << 20);
    mv = begin_box("moov");
    add_mvhd();
    add_trak_video(720, 306, 77, 0x40, false, 3u << 20, false);
    end_box(mv);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_PLAY_OK, "t02 verdict (Main+cs1 plays)");
    CHECK(w == 720 && h == 306, "t02 dimensions");
    CHECK(g_requests <= 6, "t02 few requests");
    CHECK(g_bytes < 512 * 1024, "t02 no whole-moov download");

    /* 3. same but with ctts (B-frames): rejected */
    snprintf(url, sizeof(url), "https://fake/t03-main-cs1-ctts.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    add_mdat(1024);
    mv = begin_box("moov");
    add_mvhd();
    add_trak_video(720, 306, 77, 0x40, true, 0, false);
    end_box(mv);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_HIGH_PROFILE, "t03 ctts rejects Main");

    /* 4. re-probe the same URL: served from the verdict cache, no traffic */
    int req_before = g_requests;
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_HIGH_PROFILE, "t04 cached verdict value");
    CHECK(g_requests == req_before, "t04 cached verdict: no requests");

    /* 5. Main without constraint_set1: rejected */
    snprintf(url, sizeof(url), "https://fake/t05-main-nocs1.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    mv = begin_box("moov");
    add_mvhd();
    add_trak_video(720, 306, 77, 0x00, false, 0, false);
    end_box(mv);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_HIGH_PROFILE, "t05 Main without cs1 rejected");

    /* 6. High profile: rejected */
    snprintf(url, sizeof(url), "https://fake/t06-high.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    mv = begin_box("moov");
    add_mvhd();
    add_trak_video(1280, 720, 100, 0x00, false, 0, false);
    end_box(mv);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_HIGH_PROFILE, "t06 High rejected");

    /* 7. audio trak first (2 MB index), video second, Main+cs1: the walk
     *    must skip the audio index by header alone */
    snprintf(url, sizeof(url), "https://fake/t07-audio-first.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    add_mdat(1u << 20);
    mv = begin_box("moov");
    add_mvhd();
    add_trak_audio(2u << 20);
    add_trak_video(624, 336, 77, 0x40, false, 0, false);
    end_box(mv);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_PLAY_OK, "t07 verdict (audio-first)");
    CHECK(w == 624 && h == 336, "t07 dimensions");
    CHECK(g_requests <= 6, "t07 few requests");
    CHECK(g_bytes < 512 * 1024, "t07 no whole-moov download");

    /* 8. mpeg4-asp and HEVC entries */
    snprintf(url, sizeof(url), "https://fake/t08-mp4v.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    mv = begin_box("moov");
    add_mvhd();
    add_trak_video(640, 272, 66, 0, false, 0, false);
    end_box(mv);
    /* swap the entry to mp4v via a dedicated build */
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    mv = begin_box("moov");
    add_mvhd();
    {
        size_t t = begin_box("trak");
        size_t tk = begin_box("tkhd"); add_zeros(8); end_box(tk);
        size_t md = begin_box("mdia");
        size_t mh = begin_box("mdhd"); add_zeros(8); end_box(mh);
        size_t hl = begin_box("hdlr"); add_hdlr("vide"); end_box(hl);
        size_t mi = begin_box("minf");
        size_t st = begin_box("stbl");
        add_stsd("mp4v", 640, 272, 0, 0, 0);
        add_stts();
        add_stco();
        end_box(st);
        end_box(mi);
        end_box(md);
        end_box(t);
    }
    end_box(mv);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_NOT_H264, "t08 mp4v rejected");

    /* 9. no moov at all */
    snprintf(url, sizeof(url), "https://fake/t09-nomoov.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    add_mdat(4096);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_NO_MP4, "t09 no moov -> NO_MP4");

    /* 10. transient transport failure on the first request: one retry
     *     saves it (films_probe_url retry semantics) */
    snprintf(url, sizeof(url), "https://fake/t10-retry.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    mv = begin_box("moov");
    add_mvhd();
    add_trak_video(640, 272, 66, 0xC0, false, 0, false);
    end_box(mv);
    reset_http();
    g_fail_open_at = 0;
    r = probe(url, &w, &h);
    CHECK(r == FILMS_PLAY_OK, "t10 retry recovers");

    /* 11. server ignores Range: head read is 200, moov not inside ->
     *     NO_RANGE (not garbage-derived verdicts) */
    snprintf(url, sizeof(url), "https://fake/t11-norange.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    add_mdat(256 * 1024);
    mv = begin_box("moov");
    add_mvhd();
    add_trak_video(640, 272, 66, 0xC0, false, 0, false);
    end_box(mv);
    reset_http();
    g_no_range = true;
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_NO_RANGE, "t11 200-without-range -> NO_RANGE");

    /* 12. largesize (64-bit) moov header */
    snprintf(url, sizeof(url), "https://fake/t12-largesize.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    mv = begin_box64("moov");
    add_mvhd();
    add_trak_video(640, 272, 66, 0xC0, false, 0, false);
    end_box(mv);
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_PLAY_OK, "t12 largesize moov");
    CHECK(w == 640 && h == 272, "t12 dimensions");

    /* 13. moov claims an insane size: local rejection, not a hang */
    snprintf(url, sizeof(url), "https://fake/t13-bigmoov.mp4");
    g_pos = 0; g_file_len = 0;
    add_ftyp();
    {
        size_t b = begin_box("moov");
        /* patch the size claim to 70 MB - far above the sanity limit */
        uint8_t sb[4] = { 0x04, 0x28, 0x00, 0x00 };   /* 69730336+8 */
        memcpy(g_file + b, sb, 4);
        add_mvhd();
    }
    reset_http();
    r = probe(url, &w, &h);
    CHECK(r == FILMS_ERR_BIG_MOOV, "t13 insane moov size -> BIG_MOOV");

    /* 14. real ffmpeg files (real layouts). real_fast: faststart Baseline.
     *     real_main: tail moov, x264 Main (constraint flags 0x00, B-frames
     *     present) -> rejected. Same file with constraint_set1 patched in
     *     the avcC and B-frames removed at encode time (-bf 0) -> plays.
     *     real_high: High -> rejected. */
    snprintf(url, sizeof(url), "https://fake/t14-real-fast.mp4");
    if (load_real(dir, "real_fast.mp4")) {
        reset_http();
        r = probe(url, &w, &h);
        CHECK(r == FILMS_PLAY_OK, "t14 real faststart Baseline plays");
        CHECK(w == 640 && h == 272, "t14 real dimensions");
    }

    snprintf(url, sizeof(url), "https://fake/t15-real-main.mp4");
    if (load_real(dir, "real_main.mp4")) {
        reset_http();
        r = probe(url, &w, &h);
        CHECK(r == FILMS_ERR_HIGH_PROFILE, "t15 real Main (no cs1) rejected");
        CHECK(w == 720 && h == 306, "t15 real dimensions");
    }

    snprintf(url, sizeof(url), "https://fake/t16-real-main-cs1.mp4");
    if (load_real(dir, "real_main_cs1.mp4")) {
        reset_http();
        r = probe(url, &w, &h);
        CHECK(r == FILMS_PLAY_OK, "t16 real Main+cs1 (bf=0) plays");
        CHECK(w == 720 && h == 306, "t16 real dimensions");
    }

    snprintf(url, sizeof(url), "https://fake/t17-real-main-cs1-ctts.mp4");
    if (load_real(dir, "real_main_cs1_ctts.mp4")) {
        reset_http();
        r = probe(url, &w, &h);
        CHECK(r == FILMS_ERR_HIGH_PROFILE, "t17 real Main+cs1 with B-frames rejected");
    }

    snprintf(url, sizeof(url), "https://fake/t18-real-high.mp4");
    if (load_real(dir, "real_high.mp4")) {
        reset_http();
        r = probe(url, &w, &h);
        CHECK(r == FILMS_ERR_HIGH_PROFILE, "t18 real High rejected");
    }

    printf("films host test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

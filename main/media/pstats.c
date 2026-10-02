/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser playback statistics collector + video OSD. See pstats.h.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "lwip/netif.h"
#include "lwip/pbuf.h"

#include "esp_player_types.h"

#include "app_overlay.h"        /* shared 8x8 glyph tables */
#include "camos/ui.h"           /* ui_fill_rect for the OSD backdrop */
#include "media/pstats.h"
#include "media/media_player.h" /* media_query_play() */

#define TAG "pstats"

#define PSTATS_PERIOD_MS  1000
#define PSTATS_TASK_STACK 4096
#define PSTATS_TASK_PRIO  3

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

/* Event task -> sampler (aligned 32-bit, single writers) */
static volatile bool     s_ev_buffering;
static volatile bool     s_ev_playing;
static volatile bool     s_played_once;   /* pre-buffer != re-buffer */
static volatile uint32_t s_stalls;

/* Render task -> sampler (pstats_note_video_frame) */
static volatile uint32_t s_frames;
static volatile uint32_t s_fps_target;

/* lwIP rx hook -> sampler */
static volatile uint32_t s_rx_bytes;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pstats_snapshot_t s_snap;
static volatile bool s_osd_on;

static TaskHandle_t s_task;
static bool s_inited;

/* ------------------------------------------------------------------ */
/* NIC RX byte counter: runtime wrap of netif->input (no sdkconfig)    */
/* ------------------------------------------------------------------ */

static err_t (*s_orig_input)(struct pbuf *p, struct netif *inp);

static err_t pstats_netif_input(struct pbuf *p, struct netif *inp)
{
    if (p != NULL) {
        s_rx_bytes += p->tot_len;
    }
    return s_orig_input(p, inp);
}

/* Called every sampler tick: (re)wraps netif->input with our byte counter.
 * Self-heals after a link flap - esp_netif resets ->input when the
 * interface comes up again, so an already-wrapped check is not enough. */
static void pstats_net_attach(void)
{
    struct netif *n = netif_default;
    if (n == NULL || n->input == pstats_netif_input) {
        return;
    }
    s_orig_input = n->input;
    n->input = pstats_netif_input;
    ESP_LOGI(TAG, "netif RX counter attached");
}

/* ------------------------------------------------------------------ */
/* Player event hooks (from media_player event callback)               */
/* ------------------------------------------------------------------ */

void pstats_player_event(int event_type)
{
    switch ((esp_player_event_type_t)event_type) {
    case ESP_PLAYER_EVENT_PLAYED:
        s_ev_playing = true;
        s_played_once = true;
        break;
    case ESP_PLAYER_EVENT_BUFFERING:
        if (!s_ev_buffering && s_played_once) {
            s_stalls++;             /* re-buffering, not the first fill */
        }
        s_ev_buffering = true;
        break;
    case ESP_PLAYER_EVENT_BUFFERED:
        s_ev_buffering = false;
        break;
    case ESP_PLAYER_EVENT_PAUSED:
        s_ev_playing = false;
        break;
    case ESP_PLAYER_EVENT_STOPPED:
    case ESP_PLAYER_EVENT_FINISHED:
    case ESP_PLAYER_EVENT_ERROR:
        s_ev_playing = false;
        s_ev_buffering = false;
        break;
    default:
        break;
    }
}

void pstats_stream_reset(void)
{
    s_ev_buffering = false;
    s_ev_playing = false;
    s_played_once = false;
    s_stalls = 0;
    s_frames = 0;
    s_fps_target = 0;
}

void pstats_note_video_frame(uint32_t fps)
{
    s_frames++;
    if (fps != 0) {
        s_fps_target = fps;
    }
}

void pstats_osd_set(bool on)
{
    s_osd_on = on;
}

bool pstats_osd_get(void)
{
    return s_osd_on;
}

/* ------------------------------------------------------------------ */
/* Sampler task                                                        */
/* ------------------------------------------------------------------ */

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#include "freertos/idf_additions.h"
#define PSTATS_HAVE_CPU 1
#else
#define PSTATS_HAVE_CPU 0
#endif

static uint32_t pstats_cpu_load(BaseType_t core)
{
#if PSTATS_HAVE_CPU
    configRUN_TIME_COUNTER_TYPE idle_pct = ulTaskGetIdleRunTimePercentForCore(core);
    uint32_t load = 100u - (uint32_t)idle_pct;
    return (load > 100u) ? 100u : load;
#else
    (void)core;
    return PSTATS_CPU_NA;
#endif
}

static void pstats_sample(void)
{
    static int64_t s_last_t;
    static uint32_t s_last_rx;
    static uint32_t s_last_frames;
    static uint64_t s_last_pos;
    static bool s_have_prev;

    int64_t now = esp_timer_get_time();
    pstats_snapshot_t sn = { 0 };

    if (!s_have_prev) {
        s_last_t = now;
        s_last_rx = s_rx_bytes;
        s_last_frames = s_frames;
        s_have_prev = true;
        return;
    }

    int64_t dt = now - s_last_t;            /* us */
    if (dt < 100000) {
        return;
    }

    /* --- network RX rate (KB/s, rounded) ---------------------------- */
    uint32_t rx = s_rx_bytes;
    uint32_t drx = rx - s_last_rx;          /* unsigned wrap is fine */
    sn.net_kbps = (uint32_t)(((uint64_t)drx * 1000000ULL) / ((uint64_t)dt * 1024ULL));

    /* --- CPU load per core ------------------------------------------ */
    sn.cpu0 = pstats_cpu_load(0);
    sn.cpu1 = pstats_cpu_load(1);

    /* --- render fps -------------------------------------------------- */
    uint32_t fr = s_frames;
    sn.fps_now = (uint32_t)(((uint64_t)(fr - s_last_frames) * 1000000ULL) / (uint64_t)dt);
    sn.fps_target = s_fps_target;

    /* --- buffering --------------------------------------------------- */
    sn.stalls = s_stalls;
    sn.buffering_now = s_ev_buffering;

    /* --- media clock rate -------------------------------------------- */
    uint64_t pos = 0, dur = 0;
    bool have_pos = media_query_play(&pos, &dur);
    if (have_pos) {
        sn.pos_s = (uint32_t)(pos / 1000u);
        sn.dur_s = (uint32_t)(dur / 1000u);
        uint64_t dpos = pos - s_last_pos;   /* ms of media per dt of wall */
        if (s_ev_playing && !s_ev_buffering && dpos <= (uint64_t)(dt / 1000ULL) * 2ULL) {
            sn.rate_x100 = (uint32_t)((dpos * 100ULL) / (uint64_t)(dt / 1000ULL));
            if (sn.rate_x100 > 200u) {
                sn.rate_x100 = 200u;
            }
        }
        s_last_pos = pos;
    }

    sn.seq = s_snap.seq + 1;

    portENTER_CRITICAL(&s_lock);
    s_snap = sn;
    portEXIT_CRITICAL(&s_lock);

    s_last_t = now;
    s_last_rx = rx;
    s_last_frames = fr;
}

static void pstats_task(void *arg)
{
    (void)arg;
    for (;;) {
        pstats_net_attach();
        pstats_sample();
        vTaskDelay(pdMS_TO_TICKS(PSTATS_PERIOD_MS));
    }
}

void pstats_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;
    pstats_stream_reset();
    if (xTaskCreatePinnedToCore(pstats_task, "pstats", PSTATS_TASK_STACK,
                                NULL, PSTATS_TASK_PRIO, &s_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "sampler task not created, stats disabled");
        s_inited = false;
    }
}

bool pstats_get(pstats_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }
    bool ok;
    portENTER_CRITICAL(&s_lock);
    *out = s_snap;
    ok = s_snap.seq != 0;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* OSD drawing (render task, every frame)                              */
/* ------------------------------------------------------------------ */

#define OSD_SCALE   2
#define OSD_CHAR_W  (8 * OSD_SCALE)
#define OSD_LINE_H  (8 * OSD_SCALE + 4)
#define OSD_PAD     10

/* Decode one UTF-8 code point (Cyrillic BMP is 2 bytes; everything we
 * print is ASCII or U+0400..). Returns pointer past the char. */
static const char *osd_next_cp(const char *p, uint32_t *cp)
{
    unsigned char c = (unsigned char)*p;
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        return p + 2;
    }
    *cp = c;
    return p + 1;
}

static const uint8_t *osd_glyph(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) {
        return app_overlay_font8x8[cp - 0x20];
    }
    if (cp >= 0x0410 && cp <= 0x044F) {
        return app_overlay_font8x8_cyr[cp - 0x0410];
    }
    if (cp == 0x0401) {
        return app_overlay_font8x8_cyr[64];     /* Ё */
    }
    if (cp == 0x0451) {
        return app_overlay_font8x8_cyr[65];     /* ё */
    }
    return app_overlay_font8x8['?' - 0x20];
}

/* Scale-2 text blit straight into the framebuffer (no per-pixel helper
 * calls: the render task pays for this on every video frame). */
static void osd_blit(uint16_t *fb, int fb_w, int fb_h,
                     int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    for (const char *p = s; *p != '\0'; x += OSD_CHAR_W) {
        uint32_t cp;
        p = osd_next_cp(p, &cp);
        const uint8_t *g = osd_glyph(cp);
        for (int row = 0; row < 8; row++) {
            int py = y + row * OSD_SCALE;
            if (py < 0 || py + OSD_SCALE > fb_h) {
                continue;
            }
            uint16_t *line = fb + (size_t)py * fb_w;
            for (int col = 0; col < 8; col++) {
                uint16_t color = ((g[row] >> col) & 0x1) ? fg : bg;
                for (int k = 0; k < OSD_SCALE; k++) {
                    int px = x + col * OSD_SCALE + k;
                    if (px >= 0 && px < fb_w) {
                        line[px] = color;
                    }
                }
            }
        }
    }
}

static void osd_fmt_mmss(char *out, size_t n, uint32_t sec)
{
    if (sec >= 3600u) {
        snprintf(out, n, "%lu:%02lu:%02lu",
                 (unsigned long)(sec / 3600u),
                 (unsigned long)((sec / 60u) % 60u),
                 (unsigned long)(sec % 60u));
    } else {
        snprintf(out, n, "%lu:%02lu",
                 (unsigned long)(sec / 60u), (unsigned long)(sec % 60u));
    }
}

void pstats_osd_draw(uint16_t *fb, int fb_w, int fb_h)
{
    if (fb == NULL || !s_osd_on) {
        return;
    }
    pstats_snapshot_t sn;
    if (!pstats_get(&sn)) {
        return;
    }

    char l1[48], l2[48], l3[48], l4[48], l5[48];

    snprintf(l1, sizeof(l1), "СЕТЬ %lu кБ/с",
             (unsigned long)sn.net_kbps);

    if (sn.cpu0 == PSTATS_CPU_NA) {
        strlcpy(l2, "CPU n/a", sizeof(l2));
    } else {
        snprintf(l2, sizeof(l2), "CPU0 %lu%%  CPU1 %lu%%",
                 (unsigned long)sn.cpu0, (unsigned long)sn.cpu1);
    }

    char rate[16] = "";
    if (sn.rate_x100 > 0) {
        snprintf(rate, sizeof(rate), " x%lu.%02lu",
                 (unsigned long)(sn.rate_x100 / 100u),
                 (unsigned long)(sn.rate_x100 % 100u));
    }
    snprintf(l3, sizeof(l3), "КАДР %lu/%lu%s",
             (unsigned long)sn.fps_now, (unsigned long)sn.fps_target, rate);

    if (sn.buffering_now) {
        strlcpy(l4, "СТОПЫ: БУФЕРИЗАЦИЯ", sizeof(l4));
    } else if (sn.stalls > 0) {
        snprintf(l4, sizeof(l4), "СТОПЫ %lu", (unsigned long)sn.stalls);
    } else {
        l4[0] = '\0';
    }

    if (sn.dur_s > 0) {
        char t1[16], t2[16];
        osd_fmt_mmss(t1, sizeof(t1), sn.pos_s);
        osd_fmt_mmss(t2, sizeof(t2), sn.dur_s);
        snprintf(l5, sizeof(l5), "%s / %s", t1, t2);
    } else {
        l5[0] = '\0';
    }

    /* box in the top-right corner */
    const char *lines[5] = { l1, l2, l3, l4, l5 };
    const uint16_t colors[5] = {
        (sn.net_kbps != 0 && sn.net_kbps < 50) ? UI_COLOR_WARN : UI_COLOR_OK,
        (sn.cpu1 != PSTATS_CPU_NA && sn.cpu1 > 90) ? UI_COLOR_WARN : UI_COLOR_FG,
        (sn.fps_target != 0 && sn.fps_now * 2 < sn.fps_target) ? UI_COLOR_WARN : UI_COLOR_FG,
        UI_COLOR_WARN,
        UI_COLOR_DIM,
    };

    int max_chars = 0;
    for (int i = 0; i < 5; i++) {
        int w = 0;
        for (const char *p = lines[i]; *p != '\0';) {
            uint32_t cp;
            p = osd_next_cp(p, &cp);
            w++;
        }
        if (w > max_chars) {
            max_chars = w;
        }
    }
    if (max_chars == 0) {
        max_chars = 10;     /* keep a placeholder box for l1/l2 */
    }

    int box_w = max_chars * OSD_CHAR_W + 2 * OSD_PAD;
    int box_h = 5 * OSD_LINE_H + 2 * OSD_PAD - 4;
    int x1 = fb_w - 8;
    int y0 = 10;
    int x0 = x1 - box_w;
    int y1 = y0 + box_h;
    if (x0 < 0 || y1 > fb_h) {
        return;             /* panel too small - never clip blindly */
    }

    ui_fill_rect(fb, fb_w, fb_h, x0, y0, x1, y1, 0x0000);

    int ty = y0 + OSD_PAD;
    for (int i = 0; i < 5; i++) {
        if (lines[i][0] != '\0') {
            /* right-align: text grows leftward from the box's right pad */
            int wpx = 0;
            for (const char *p = lines[i]; *p != '\0';) {
                uint32_t cp;
                p = osd_next_cp(p, &cp);
                wpx += OSD_CHAR_W;
            }
            osd_blit(fb, fb_w, fb_h, x1 - OSD_PAD - wpx, ty,
                     lines[i], colors[i], 0x0000);
        }
        ty += OSD_LINE_H;
    }
}

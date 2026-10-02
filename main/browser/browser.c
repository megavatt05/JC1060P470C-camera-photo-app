/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser: touch-driven text browser on the JD9165 1024x600 LCD.
 *
 * State machine (single FreeRTOS task, core 1; the camera stream task is
 * stopped before this task owns the screen, see docs/ETHERNET_BROWSER.md):
 *
 *   SPLASH ──(net ready / tap)──▶ HOME (query + keyboard)
 *   HOME ──GO──▶ LOADING ──▶ RESULTS ──tap──▶ PAGELOAD ──▶ PAGE
 *   RESULTS/PAGE ──nav bar──▶ HOME / back / scroll
 *
 * All drawing goes through camos/ui.h into the DPI panel frame buffers
 * (app_lcd_get_fb / app_lcd_flush, double-buffered full-screen flush).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "app_lcd.h"
#include "net/app_eth.h"
#include "camos/ui.h"
#include "camos/touch.h"
#include "browser/web_client.h"
#include "browser/html_text.h"
#include "browser/browser.h"
#if CONFIG_EB_MEDIA_ENABLE
#include "media/media_player.h"
#include "media/sdcard.h"
#include <dirent.h>
#endif

static const char *TAG = "browser";

/* --- Layout -------------------------------------------------------------- */

#define LCD_W           EXAMPLE_LCD_H_RES
#define LCD_H           EXAMPLE_LCD_V_RES

#define BAR_H           32      /* top status bar          */
#define URL_Y           (BAR_H) /* query row               */
#define URL_H           56
#define GO_W            110

#define CONTENT_Y0      (BAR_H + URL_H + 4)
#define CONTENT_Y1      368     /* keyboard starts below   */
#define CONTENT_Y1_FULL (LCD_H - 64) /* without keyboard   */

#define ROW_H           36      /* results list row        */
#define LINE_H          20      /* page text line          */
#define PAGE_COLS       ((LCD_W - 24) / (UI_FONT_W * UI_SCALE_TEXT))

/* Keyboard geometry: 5 rows of 80x44 keys, gaps 2 px vertical /
 * 4 px horizontal, centered.  Key labels draw at KB_KEY_SCALE so a glyph
 * (32x32 px) fills most of the button face.  Worst row is 12 keys:
 * 12 * 80 + 11 * 4 = 1004 <= LCD_W (1024).  Five rows of 44 + 4 gaps of 2
 * = 228 px, so the block spans CONTENT_Y1 + 2 .. 598 on a 600 px panel. */
#define KEY_W           80
#define KEY_H           44
#define KEY_GAP         4
#define KEY_VGAP        2
#define KB_KEY_SCALE    4

/* Button ids. All stay below BTN_CHAR (0x20): ids >= BTN_CHAR are
 * literal keyboard codepoints. */
enum {
    BTN_NONE = 0,
    BTN_GO = 1,
    BTN_ENGINE,
    BTN_SPC,
    BTN_DEL,
    BTN_HOME,
    BTN_BACK,
    BTN_UP,
    BTN_DOWN,
    BTN_KBD,
    BTN_URLROW,
    BTN_RADIO,
    BTN_VIDEO,
    BTN_MSTOP,
    BTN_MPAUSE,
    BTN_VOLUP,
    BTN_VOLDN,
    BTN_MBACK,
    BTN_MURL,
    BTN_SD,                 /* open the SD file list        */
    BTN_FREFR,              /* rescan the card              */
    BTN_FUP,
    BTN_FDOWN,
    BTN_FILEROW,            /* row = (y - SD_ROWS_Y0)/SD_ROW_STRIDE */
    BTN_STATION,            /* + 0..RADIO_STATIONS_N-1 */
    BTN_VIDPRESET,          /* + 0..VIDEO_PRESETS_N-1 */
    BTN_CHAR = 0x20,        /* ids >= BTN_CHAR are literal chars */
};

typedef enum {
    ST_SPLASH,
    ST_HOME,
    ST_LOADING,
    ST_RESULTS,
    ST_PAGELOAD,
    ST_PAGE,
#if CONFIG_EB_MEDIA_ENABLE
    ST_RADIO,               /* radio screen (stations + controls)      */
    ST_VIDEO,               /* video screen (presets + custom URL)     */
    ST_URLIN,               /* keyboard: type a media URL              */
    ST_VIDEOP,              /* video playing: the render owns the panel */
    ST_FILES,               /* media files on the SD card              */
#endif
} browser_state_t;

/* --- Static data ---------------------------------------------------------- */

static struct {
    int fb_idx;
    browser_state_t state;

    char query[96];
    bool engine_google;
    bool kb_ru;             /* on-screen keyboard layout: false=EN true=RU */

    web_result_t results[WEB_RESULTS_MAX];
    int result_count;
    bool serp_error;

    char *page_text;        /* PSRAM plain text of the opened page */
    int  page_scroll;

    /* wrapped page lines (pointers into page_text) */
    struct { const char *s; int len; } *lines;
    int line_count;

    int splash_attempts;
#if CONFIG_EB_MEDIA_ENABLE
    bool urlin_video;       /* ST_URLIN target: false=radio true=video  */
#endif
} br;

/* --- Wrapping ------------------------------------------------------------- */

static void page_wrap(void)
{
    if (br.lines == NULL) {
        br.lines = heap_caps_malloc(4096 * sizeof(*br.lines), MALLOC_CAP_SPIRAM);
        if (br.lines == NULL) {
            return;
        }
    }
    br.line_count = 0;

    const char *p = br.page_text;
    if (p == NULL) {
        return;
    }

    while (*p != '\0' && br.line_count < 4090) {
        const char *line_start = p;
        const char *last_space = NULL;
        int col = 0;

        while (*p != '\0' && *p != '\n' && col < PAGE_COLS) {
            if (*p == ' ') {
                last_space = p;
            }
            if ((((unsigned char)*p) & 0xC0) != 0x80) {
                col++;          /* count codepoints, not UTF-8 bytes */
            }
            p++;
        }
        /* never split a UTF-8 sequence at the wrap boundary */
        while (p > line_start && (((unsigned char)*p) & 0xC0) == 0x80) {
            p--;
        }

        if (*p == '\n') {
            br.lines[br.line_count].s = line_start;
            br.lines[br.line_count].len = (int)(p - line_start);
            br.line_count++;
            p++;
            continue;
        }
        if (*p != '\0' && last_space != NULL && last_space > line_start) {
            p = last_space;             /* wrap at the space */
            br.lines[br.line_count].s = line_start;
            br.lines[br.line_count].len = (int)(p - line_start);
            br.line_count++;
            p++;                        /* skip the space */
        } else {
            br.lines[br.line_count].s = line_start;
            br.lines[br.line_count].len = (int)(p - line_start);
            br.line_count++;
        }
    }
}

static void page_free(void)
{
    if (br.page_text != NULL) {
        free(br.page_text);
        br.page_text = NULL;
    }
    br.line_count = 0;
    br.page_scroll = 0;
}

/* --- Drawing helpers -------------------------------------------------------- */

static void draw_status_bar(uint16_t *fb)
{
    char left[48];
    char right[40];

    ui_fill_rect(fb, LCD_W, LCD_H, 0, 0, LCD_W, BAR_H, UI_COLOR_BAR);

    if (app_eth_ready()) {
        snprintf(left, sizeof(left), "ETH: %s", app_eth_ip_str());
    } else {
        snprintf(left, sizeof(left), "NET: %s", app_eth_status_str());
    }
    snprintf(right, sizeof(right), "[%s] touch:%s",
             br.engine_google ? "GGL" : "DDG", touch_chip_name());
#if CONFIG_EB_MEDIA_ENABLE
    if (media_get_state() == MEDIA_STATE_PLAYING && !media_video_active()) {
        strlcat(right, " R", sizeof(right));    /* radio plays in background */
    }
#endif

    ui_text(fb, LCD_W, LCD_H, 8, (BAR_H - UI_FONT_H) / 2,
            left, 1, UI_COLOR_OK, UI_COLOR_BAR);
    ui_text(fb, LCD_W, LCD_H, LCD_W - ui_text_width(right, 1) - 8,
            (BAR_H - UI_FONT_H) / 2, right, 1, UI_COLOR_URL, UI_COLOR_BAR);
}

static void draw_url_row(uint16_t *fb)
{
    static const ui_button_t go = {
        .x = LCD_W - GO_W - 8, .y = URL_Y + 4, .w = GO_W, .h = URL_H - 8,
        .label = "GO", .id = BTN_GO, .scale = KB_KEY_SCALE
    };

    ui_fill_rect(fb, LCD_W, LCD_H, 0, URL_Y, LCD_W, URL_Y + URL_H, UI_COLOR_BG);
    ui_frame(fb, LCD_W, LCD_H, 4, URL_Y + 2, LCD_W - GO_W - 12, URL_Y + URL_H - 2,
             2, UI_COLOR_BORDER);

    char fitted[96];
    ui_text_fit(fitted, sizeof(fitted), br.query, LCD_W - GO_W - 40, UI_SCALE_URL);
    ui_text(fb, LCD_W, LCD_H, 10, URL_Y + (URL_H - UI_FONT_H * UI_SCALE_URL) / 2,
            fitted, UI_SCALE_URL, UI_COLOR_FG, UI_COLOR_BG);

    /* caret */
    int cx = 10 + ui_text_width(fitted, UI_SCALE_URL) + 2;
    if ((esp_timer_get_time() / 500000) & 1) {
        ui_fill_rect(fb, LCD_W, LCD_H, cx, URL_Y + 16, cx + UI_SCALE_URL,
                     URL_Y + URL_H - 16, UI_COLOR_ACCENT);
    }

    ui_button(fb, LCD_W, LCD_H, &go, false);
}

static void draw_nav_bar(uint16_t *fb)
{
    static const ui_button_t nav[] = {
        { 8,   LCD_H - 60, 180, 56, "HOME",  BTN_HOME },
        { 196, LCD_H - 60, 180, 56, "BACK",  BTN_BACK },
        { 830, LCD_H - 60, 88,  56, "UP",    BTN_UP   },
        { 926, LCD_H - 60, 88,  56, "DOWN",  BTN_DOWN },
    };

    ui_fill_rect(fb, LCD_W, LCD_H, 0, LCD_H - 64, LCD_W, LCD_H, UI_COLOR_BG);
    for (size_t i = 0; i < sizeof(nav) / sizeof(nav[0]); i++) {
        ui_button(fb, LCD_W, LCD_H, &nav[i], false);
    }
}

static bool nav_hit(int x, int y, int *out_id)
{
    static const ui_button_t nav[] = {
        { 8,   LCD_H - 60, 180, 56, "HOME",  BTN_HOME },
        { 196, LCD_H - 60, 180, 56, "BACK",  BTN_BACK },
        { 830, LCD_H - 60, 88,  56, "UP",    BTN_UP   },
        { 926, LCD_H - 60, 88,  56, "DOWN",  BTN_DOWN },
    };
    for (size_t i = 0; i < sizeof(nav) / sizeof(nav[0]); i++) {
        if (ui_button_hit(&nav[i], x, y)) {
            *out_id = nav[i].id;
            return true;
        }
    }
    return false;
}

/* Tap on the GO button in the URL row (top of the screen) */
static bool urlrow_go_hit(int x, int y)
{
    return (y >= URL_Y && y < URL_Y + URL_H &&
            x >= LCD_W - GO_W - 8 && x < LCD_W - 8);
}

/* --- Keyboard ---------------------------------------------------------------- */

/* Keys are Unicode codepoints: ASCII prints as-is, Cyrillic (U+0410..)
 * is UTF-8 encoded by query_append_cp() and rendered with the Cyrillic
 * glyph table in app_overlay_font8x8_cyr. 0 terminates a row. */

#define KB_ROWS_NUM 4
#define KB_ROW_MAX  13

static const uint32_t KB_EN[KB_ROWS_NUM][KB_ROW_MAX] = {
    { '1','2','3','4','5','6','7','8','9','0','-','_', 0 },
    { 'q','w','e','r','t','y','u','i','o','p', 0 },
    { 'a','s','d','f','g','h','j','k','l','.', 0 },
    { 'z','x','c','v','b','n','m','_','/', 0 },
};

/* ЙЦУКЕН: ё on the digit row, standard letter rows, '.'/"/" kept */
static const uint32_t KB_RU[KB_ROWS_NUM][KB_ROW_MAX] = {
    { 0x0451,'1','2','3','4','5','6','7','8','9','0','-', 0 },
    { 0x0439,0x0446,0x0443,0x043A,0x0435,0x043D,0x0433,0x0448,0x0449,0x0437,0x0445,0x044A, 0 },
    { 0x0444,0x044B,0x0432,0x0430,0x043F,0x0440,0x043E,0x043B,0x0434,0x0436,0x044D, 0 },
    { 0x044F,0x0447,0x0441,0x043C,0x0438,0x0442,0x044C,0x0431,0x044E,'.', 0 },
};

static const uint32_t (*kb_rows(void))[KB_ROW_MAX]
{
    return br.kb_ru ? KB_RU : KB_EN;
}

/* UTF-8 encode one key codepoint into buf (3 bytes); returns byte length */
static int kb_key_label(uint32_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        buf[1] = '\0';
        return 1;
    }
    buf[0] = (char)(0xC0 | (cp >> 6));
    buf[1] = (char)(0x80 | (cp & 0x3F));
    buf[2] = '\0';
    return 2;
}

/* Bottom function row: SPC / DEL / EN|RU / engine / GO.  Shared by
 * draw_keyboard() and keyboard_hit() so drawing and hit-testing can never
 * diverge.  NULL labels are resolved from the runtime state. */
static const struct { int w; const char *label; int id; } kb_bottom[] = {
    { 200, "SPC", BTN_SPC },
    { 150, "DEL", BTN_DEL },
    { 150, NULL,  BTN_KBD },        /* EN | RU                    */
    { 170, NULL,  BTN_ENGINE },     /* GGL | DDG                  */
    { 170, "GO",  BTN_GO },
};
#define KB_BOTTOM_N (sizeof(kb_bottom) / sizeof(kb_bottom[0]))

static const char *kb_bottom_label(size_t i)
{
    if (kb_bottom[i].label != NULL) {
        return kb_bottom[i].label;
    }
    if (kb_bottom[i].id == BTN_KBD) {
        return br.kb_ru ? "RU" : "EN";
    }
#if CONFIG_EB_MEDIA_ENABLE
    /* on the media URL input screen the engine slot doubles as CANCEL */
    if (kb_bottom[i].id == BTN_ENGINE && br.state == ST_URLIN) {
        return "НАЗАД";
    }
#endif
    return br.engine_google ? "GGL" : "DDG";
}

static int kb_bottom_x0(void)
{
    int total = 0;
    for (size_t i = 0; i < KB_BOTTOM_N; i++) {
        total += kb_bottom[i].w;
    }
    total += (int)(KB_BOTTOM_N - 1) * KEY_GAP;
    return (LCD_W - total) / 2;
}

static void draw_keyboard(uint16_t *fb)
{
    ui_fill_rect(fb, LCD_W, LCD_H, 0, CONTENT_Y1, LCD_W, LCD_H, 0x0000);

    const uint32_t (*rows)[KB_ROW_MAX] = kb_rows();
    int y = CONTENT_Y1 + KEY_VGAP;
    for (int r = 0; r < KB_ROWS_NUM; r++) {
        int n = 0;
        while (n < KB_ROW_MAX && rows[r][n] != 0) {
            n++;
        }
        int row_w = n * KEY_W + (n - 1) * KEY_GAP;
        int x = (LCD_W - row_w) / 2;

        for (int k = 0; k < n; k++, x += KEY_W + KEY_GAP) {
            char lab[3];
            kb_key_label(rows[r][k], lab);
            ui_button_t key = { .x = x, .y = y, .w = KEY_W, .h = KEY_H,
                                .label = lab, .id = (int)rows[r][k],
                                .scale = KB_KEY_SCALE };
            ui_button(fb, LCD_W, LCD_H, &key, false);
        }
        y += KEY_H + KEY_VGAP;
    }

    int x = kb_bottom_x0();
    for (size_t i = 0; i < KB_BOTTOM_N; i++) {
        ui_button_t key = { .x = x, .y = y, .w = kb_bottom[i].w, .h = KEY_H,
                            .label = kb_bottom_label(i), .id = kb_bottom[i].id,
                            .scale = KB_KEY_SCALE };
        ui_button(fb, LCD_W, LCD_H, &key, false);
        x += kb_bottom[i].w + KEY_GAP;
    }
}

/* Returns BTN_* id or the key codepoint (>= BTN_CHAR), or 0 on a miss */
static int keyboard_hit(int x, int y)
{
    if (y < CONTENT_Y1) {
        return BTN_NONE;
    }

    const uint32_t (*rows)[KB_ROW_MAX] = kb_rows();
    int ky = CONTENT_Y1 + KEY_VGAP;
    for (int r = 0; r < KB_ROWS_NUM; r++) {
        int n = 0;
        while (n < KB_ROW_MAX && rows[r][n] != 0) {
            n++;
        }
        int row_w = n * KEY_W + (n - 1) * KEY_GAP;
        int kx = (LCD_W - row_w) / 2;

        if (y >= ky && y < ky + KEY_H) {
            for (int k = 0; k < n; k++, kx += KEY_W + KEY_GAP) {
                if (x >= kx && x < kx + KEY_W) {
                    return (int)rows[r][k];
                }
            }
            return BTN_NONE;
        }
        ky += KEY_H + KEY_VGAP;
    }

    if (y >= ky && y < ky + KEY_H) {
        int x0 = kb_bottom_x0();
        for (size_t i = 0; i < KB_BOTTOM_N; i++) {
            if (x >= x0 && x < x0 + kb_bottom[i].w) {
                return kb_bottom[i].id;
            }
            x0 += kb_bottom[i].w + KEY_GAP;
        }
    }
    return BTN_NONE;
}

/* --- Screens ------------------------------------------------------------------ */

static void draw_screen(void);

#if CONFIG_EB_MEDIA_ENABLE

/* --- Media: internet radio + video (see media/media_player.h) --------------- */

static void draw_loading(uint16_t *fb, const char *what);

static const struct { const char *name; const char *url; } radio_stations[] = {
    { "Европа Плюс",    "http://ep128server.streamr.ru:8030/ep128" },
    { "Радио Рекорд",   "http://air.radiorecord.ru:805/rr_320" },
    { "Дорожное радио", "http://dorognoe.hostingradio.ru:8000/dorognoe" },
    { "Радио Дача",     "http://dacha.hostingradio.ru:8025/radiodacha96.aacp" },
    { "SomaFM Groove",  "https://ice1.somafm.com/groovesalad-128-mp3" },
    { "Radio Paradise", "http://stream.radioparadise.com/mp3-128" },
};
#define RADIO_STATIONS_N (sizeof(radio_stations) / sizeof(radio_stations[0]))

/* Video presets. The SW H264 decoder (esp_h264 / tinyh264) plays only
 * Constrained Baseline streams: Main/High profiles are rejected at SPS
 * validation ("H264_DEC: profile_idc is error"). The clips in media/ are
 * Big Buck Bunny re-encoded to Constrained Baseline and served via
 * raw.githubusercontent.com (supports Range requests).
 * Decode speed reality (SW decode, P4 rev<3 @360MHz): 360p@30 lands at
 * ~5 fps (slideshow); 180p@20 is roughly realtime. Keep presets small.
 * See docs/ETHERNET_BROWSER.md, section "Медиа". */
static const struct { const char *name; const char *url; } video_presets[] = {
    { "BBB 180p (плавно)", "https://raw.githubusercontent.com/megavatt05/JC1060P470C-camera-photo-app/feature/ethernet-browser/media/bbb_180p_cb.mp4" },
    { "BBB 360p (четко)",  "https://raw.githubusercontent.com/megavatt05/JC1060P470C-camera-photo-app/feature/ethernet-browser/media/bbb_360p_cb.mp4" },
};
#define VIDEO_PRESETS_N (sizeof(video_presets) / sizeof(video_presets[0]))

/* HOME: two app launcher buttons in the content zone */
static const ui_button_t home_apps[] = {
    { 110, 150, 360, 110, "РАДИО", BTN_RADIO, 3 },
    { 554, 150, 360, 110, "ВИДЕО", BTN_VIDEO, 3 },
};
#define HOME_APPS_N (sizeof(home_apps) / sizeof(home_apps[0]))

static bool home_apps_hit(int x, int y, int *out_id)
{
    for (size_t i = 0; i < HOME_APPS_N; i++) {
        if (ui_button_hit(&home_apps[i], x, y)) {
            *out_id = home_apps[i].id;
            return true;
        }
    }
    return false;
}

/* Radio: station tile geometry, shared by draw and hit-test */

static void radio_st_btn(int i, ui_button_t *b)
{
    b->x = (i & 1) ? 516 : 12;
    b->y = 88 + (i >> 1) * 80;
    b->w = 496;
    b->h = 72;
    b->label = radio_stations[i].name;
    b->id = (int)(BTN_STATION + i);
    b->scale = 3;
}

static bool media_url_norm(const char *in, char *out, size_t outsz)
{
    while (*in == ' ') {
        in++;
    }
    if (*in == '\0') {
        return false;
    }
    if (strncasecmp(in, "http://", 7) != 0 && strncasecmp(in, "https://", 8) != 0) {
        snprintf(out, outsz, "http://%s", in);
    } else {
        snprintf(out, outsz, "%s", in);
    }
    return true;
}

static void draw_radio(uint16_t *fb)
{
    draw_status_bar(fb);
    ui_fill_rect(fb, LCD_W, LCD_H, 0, BAR_H, LCD_W, LCD_H, UI_COLOR_BG);

    ui_text(fb, LCD_W, LCD_H, 12, 44, "РАДИО", UI_SCALE_TITLE, UI_COLOR_ACCENT, UI_COLOR_BG);
    char st[48];
    snprintf(st, sizeof(st), "%s  ЗВУК:%d", media_state_str(), media_get_volume());
    ui_text(fb, LCD_W, LCD_H, LCD_W - ui_text_width(st, UI_SCALE_TEXT) - 12,
            52, st, UI_SCALE_TEXT, UI_COLOR_URL, UI_COLOR_BG);

    for (size_t i = 0; i < RADIO_STATIONS_N; i++) {
        ui_button_t b;
        radio_st_btn((int)i, &b);
        ui_button(fb, LCD_W, LCD_H, &b, false);
        /* highlight the station whose stream is loaded right now */
        if (media_get_state() != MEDIA_STATE_IDLE &&
            strcmp(media_get_url(), radio_stations[i].url) == 0) {
            ui_frame(fb, LCD_W, LCD_H, b.x - 3, b.y - 3, b.x + b.w + 3, b.y + b.h + 3,
                     2, UI_COLOR_OK);
        }
    }

    static const struct { int w; int id; } ctrls[] = {
        { 180, BTN_VOLDN }, { 220, BTN_MPAUSE }, { 180, BTN_VOLUP }, { 220, BTN_MSTOP },
    };
    int x = (LCD_W - (180 + 220 + 180 + 220 + 3 * 12)) / 2;
    for (size_t i = 0; i < sizeof(ctrls) / sizeof(ctrls[0]); i++) {
        const char *lab = "ПАУЗА";
        if (ctrls[i].id == BTN_VOLDN)  lab = "ТИШЕ-";
        if (ctrls[i].id == BTN_VOLUP)  lab = "ГРОМЧЕ+";
        if (ctrls[i].id == BTN_MPAUSE) lab = (media_get_state() == MEDIA_STATE_PAUSED) ? "ПУСК" : "ПАУЗА";
        if (ctrls[i].id == BTN_MSTOP)  lab = "СТОП";
        ui_button_t b = { x, 336, ctrls[i].w, 64, lab, ctrls[i].id, 3 };
        ui_button(fb, LCD_W, LCD_H, &b, false);
        x += ctrls[i].w + 12;
    }

    ui_button_t sd = { 200, 420, 280, 60, "SD-КАРТА", BTN_SD, 3 };
    ui_button(fb, LCD_W, LCD_H, &sd, false);
    ui_button_t back = { 544, 420, 280, 60, "НАЗАД", BTN_MBACK, 3 };
    ui_button(fb, LCD_W, LCD_H, &back, false);

    if (media_get_state() != MEDIA_STATE_IDLE && media_get_url()[0] != '\0') {
        char fitted[128];
        ui_text_fit(fitted, sizeof(fitted), media_get_url(), LCD_W - 24, 1);
        ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(fitted, 1)) / 2,
                500, fitted, 1, UI_COLOR_BORDER, UI_COLOR_BG);
    }
}

static int radio_hit(int x, int y)
{
    for (size_t i = 0; i < RADIO_STATIONS_N; i++) {
        ui_button_t b;
        radio_st_btn((int)i, &b);
        if (ui_button_hit(&b, x, y)) {
            return b.id;
        }
    }
    static const struct { int w; int id; } ctrls[] = {
        { 180, BTN_VOLDN }, { 220, BTN_MPAUSE }, { 180, BTN_VOLUP }, { 220, BTN_MSTOP },
    };
    int cx = (LCD_W - (180 + 220 + 180 + 220 + 3 * 12)) / 2;
    for (size_t i = 0; i < sizeof(ctrls) / sizeof(ctrls[0]); i++) {
        if (x >= cx && x < cx + ctrls[i].w && y >= 336 && y < 336 + 64) {
            return ctrls[i].id;
        }
        cx += ctrls[i].w + 12;
    }
    ui_button_t sd = { 200, 420, 280, 60, "SD-КАРТА", BTN_SD, 3 };
    if (ui_button_hit(&sd, x, y)) {
        return BTN_SD;
    }
    ui_button_t back = { 544, 420, 280, 60, "НАЗАД", BTN_MBACK, 3 };
    if (ui_button_hit(&back, x, y)) {
        return BTN_MBACK;
    }
    return BTN_NONE;
}

static void video_preset_btn(int i, ui_button_t *b)
{
    b->x = 12;
    b->y = 92 + i * 84;
    b->w = LCD_W - 24;
    b->h = 76;
    b->label = video_presets[i].name;
    b->id = (int)(BTN_VIDPRESET + i);
    b->scale = 3;
}

static void draw_video(uint16_t *fb)
{
    draw_status_bar(fb);
    ui_fill_rect(fb, LCD_W, LCD_H, 0, BAR_H, LCD_W, LCD_H, UI_COLOR_BG);

    ui_text(fb, LCD_W, LCD_H, 12, 44, "ВИДЕО", UI_SCALE_TITLE, UI_COLOR_ACCENT, UI_COLOR_BG);

    for (size_t i = 0; i < VIDEO_PRESETS_N; i++) {
        ui_button_t b;
        video_preset_btn((int)i, &b);
        ui_button(fb, LCD_W, LCD_H, &b, false);
    }

    ui_button_t url = { 168, 356, 320, 64, "СВОЙ URL", BTN_MURL, 3 };
    ui_button(fb, LCD_W, LCD_H, &url, false);
    ui_button_t sd = { 536, 356, 320, 64, "SD-КАРТА", BTN_SD, 3 };
    ui_button(fb, LCD_W, LCD_H, &sd, false);

    ui_button_t back = { 372, 436, 280, 56, "НАЗАД", BTN_MBACK, 3 };
    ui_button(fb, LCD_W, LCD_H, &back, false);

    const char *hint = "MP4 (H.264 + AAC) по HTTP/HTTPS или с SD-карты";
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(hint, 1)) / 2,
            508, hint, 1, UI_COLOR_BORDER, UI_COLOR_BG);
}

static int video_hit(int x, int y)
{
    for (size_t i = 0; i < VIDEO_PRESETS_N; i++) {
        ui_button_t b;
        video_preset_btn((int)i, &b);
        if (ui_button_hit(&b, x, y)) {
            return b.id;
        }
    }
    ui_button_t url = { 168, 356, 320, 64, "СВОЙ URL", BTN_MURL, 3 };
    if (ui_button_hit(&url, x, y)) {
        return BTN_MURL;
    }
    ui_button_t sd = { 536, 356, 320, 64, "SD-КАРТА", BTN_SD, 3 };
    if (ui_button_hit(&sd, x, y)) {
        return BTN_SD;
    }
    ui_button_t back = { 372, 436, 280, 56, "НАЗАД", BTN_MBACK, 3 };
    if (ui_button_hit(&back, x, y)) {
        return BTN_MBACK;
    }
    return BTN_NONE;
}

/* --- SD card file list ------------------------------------------------------- */

#define SD_ROWS_Y0      92      /* first file row                  */
#define SD_ROW_H        48
#define SD_ROW_STRIDE   52
#define SD_ROWS         7       /* visible rows                    */
#define SD_FILES_MAX    32      /* scanned entries (scroll for more) */
#define SD_NAME_MAX     96

static struct {
    char name[SD_NAME_MAX];
    bool video;                 /* video file (else audio)         */
} sd_files[SD_FILES_MAX];
static int  sd_files_n;
static int  sd_scroll;
static bool sd_mount_err;
static bool sd_from_video;      /* which screen opened the list    */

/* where ST_VIDEOP returns after the playback stops */
static browser_state_t videop_back = ST_VIDEO;

/* forward decl: sd_play (below) starts videos through it */
static void video_start_ui(const char *url);

static bool sd_ext_match(const char *name, const char *const *exts, size_t n)
{
    size_t len = strlen(name);
    for (size_t i = 0; i < n; i++) {
        size_t el = strlen(exts[i]);
        if (len > el && strcasecmp(name + len - el, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Mount the card (lazy) and list media files in its root. The player
 * decodes what this build enables: MP3/AAC/WAV/M4A audio, MP4 video. */
static void sd_scan(void)
{
    sd_files_n = 0;
    sd_scroll = 0;
    sd_mount_err = false;

    if (sdcard_mount() != ESP_OK) {
        sd_mount_err = true;
        return;
    }
    DIR *d = opendir(sdcard_mp());
    if (d == NULL) {
        sd_mount_err = true;
        return;
    }
    static const char *const audio_ext[] = { ".mp3", ".wav", ".m4a", ".aac" };
    static const char *const video_ext[] = { ".mp4", ".m4v", ".mov" };
    struct dirent *e;
    while ((e = readdir(d)) != NULL && sd_files_n < SD_FILES_MAX) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || strlen(nm) >= SD_NAME_MAX) {
            continue;
        }
        bool video = sd_ext_match(nm, video_ext, sizeof(video_ext) / sizeof(video_ext[0]));
        if (!video && !sd_ext_match(nm, audio_ext, sizeof(audio_ext) / sizeof(audio_ext[0]))) {
            continue;
        }
        strlcpy(sd_files[sd_files_n].name, nm, SD_NAME_MAX);
        sd_files[sd_files_n].video = video;
        sd_files_n++;
    }
    closedir(d);

    /* FAT lists in creation order - sort alphabetically for usability */
    for (int i = 1; i < sd_files_n; i++) {
        typeof(sd_files[0]) t = sd_files[i];
        int j = i - 1;
        while (j >= 0 && strcmp(sd_files[j].name, t.name) > 0) {
            sd_files[j + 1] = sd_files[j];
            j--;
        }
        sd_files[j + 1] = t;
    }
    ESP_LOGI(TAG, "SD: %d media file(s) in %s", sd_files_n, sdcard_mp());
}

/* Shared by draw and hit-test; label buffer is static on purpose: the
 * browser task is the only caller and label must outlive the call. */
static void sd_file_btn(int vis_row, ui_button_t *b)
{
    static char lab[SD_NAME_MAX + 8];
    const typeof(sd_files[0]) *f = &sd_files[sd_scroll + vis_row];
    snprintf(lab, sizeof(lab), "%s %s", f->video ? "[V]" : "[A]", f->name);
    b->x = 12;
    b->y = SD_ROWS_Y0 + vis_row * SD_ROW_STRIDE;
    b->w = LCD_W - 24;
    b->h = SD_ROW_H;
    b->label = lab;
    b->id = BTN_FILEROW;
    b->scale = 2;
}

static void draw_files(uint16_t *fb)
{
    draw_status_bar(fb);
    ui_fill_rect(fb, LCD_W, LCD_H, 0, BAR_H, LCD_W, LCD_H, UI_COLOR_BG);

    ui_text(fb, LCD_W, LCD_H, 12, 44, "ФАЙЛЫ SD", UI_SCALE_TITLE,
            UI_COLOR_ACCENT, UI_COLOR_BG);
    char st[48];
    snprintf(st, sizeof(st), "%s  ЗВУК:%d", media_state_str(), media_get_volume());
    ui_text(fb, LCD_W, LCD_H, LCD_W - ui_text_width(st, UI_SCALE_TEXT) - 12,
            52, st, UI_SCALE_TEXT, UI_COLOR_URL, UI_COLOR_BG);

    if (sd_mount_err) {
        const char *msg = "карта не читается (FAT32?) - ОБНОВИТЬ повторит";
        ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(msg, UI_SCALE_TEXT)) / 2,
                260, msg, UI_SCALE_TEXT, UI_COLOR_ERR, UI_COLOR_BG);
    } else if (sd_files_n == 0) {
        const char *msg = "нет медиафайлов в корне карты";
        ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(msg, UI_SCALE_TEXT)) / 2,
                260, msg, UI_SCALE_TEXT, UI_COLOR_BORDER, UI_COLOR_BG);
    } else {
        int rows = sd_files_n - sd_scroll;
        if (rows > SD_ROWS) {
            rows = SD_ROWS;
        }
        for (int i = 0; i < rows; i++) {
            ui_button_t b;
            sd_file_btn(i, &b);
            ui_button(fb, LCD_W, LCD_H, &b, false);
        }
        int left = sd_files_n - sd_scroll - rows;
        if (left > 0) {
            char more[48];
            snprintf(more, sizeof(more), "листать: ВНИЗ (ещё %d)", left);
            ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(more, 1)) / 2,
                    536, more, 1, UI_COLOR_BORDER, UI_COLOR_BG);
        }
    }

    static const struct { int w; const char *lab; int id; } ctrls[] = {
        { 140, "ВВЕРХ",    BTN_FUP    }, { 200, "ОБНОВИТЬ", BTN_FREFR },
        { 140, "ВНИЗ",     BTN_FDOWN  }, { 200, "НАЗАД",    BTN_MBACK },
    };
    int x = (LCD_W - (140 + 200 + 140 + 200 + 3 * 12)) / 2;
    for (size_t i = 0; i < sizeof(ctrls) / sizeof(ctrls[0]); i++) {
        ui_button_t b = { x, 466, ctrls[i].w, 60, ctrls[i].lab, ctrls[i].id, 2 };
        ui_button(fb, LCD_W, LCD_H, &b, false);
        x += ctrls[i].w + 12;
    }
}

static int files_hit(int x, int y)
{
    if (y >= SD_ROWS_Y0 && y < SD_ROWS_Y0 + SD_ROWS * SD_ROW_STRIDE - 4 &&
        x >= 12 && x < LCD_W - 12) {
        return BTN_FILEROW;
    }
    static const struct { int w; int id; } ctrls[] = {
        { 140, BTN_FUP }, { 200, BTN_FREFR }, { 140, BTN_FDOWN }, { 200, BTN_MBACK },
    };
    int cx = (LCD_W - (140 + 200 + 140 + 200 + 3 * 12)) / 2;
    for (size_t i = 0; i < sizeof(ctrls) / sizeof(ctrls[0]); i++) {
        if (x >= cx && x < cx + ctrls[i].w && y >= 466 && y < 526) {
            return ctrls[i].id;
        }
        cx += ctrls[i].w + 12;
    }
    return BTN_NONE;
}

/* Play one file from the list (index is absolute, not scrolled) */
static void sd_play(int idx)
{
    if (idx < 0 || idx >= sd_files_n) {
        return;
    }
    char path[SD_NAME_MAX + 24];
    snprintf(path, sizeof(path), "%s/%s", sdcard_mp(), sd_files[idx].name);
    ESP_LOGI(TAG, "play from SD: %s", path);
    if (sd_files[idx].video) {
        videop_back = ST_FILES;
        video_start_ui(path);
    } else {
        media_radio_start(path);   /* audio keeps playing in background */
        draw_screen();
    }
}

static void draw_urlin(uint16_t *fb)
{
    draw_status_bar(fb);
    draw_url_row(fb);

    ui_fill_rect(fb, LCD_W, LCD_H, 0, CONTENT_Y0, LCD_W, CONTENT_Y1, UI_COLOR_BG);
    const char *target = br.urlin_video ? "адрес видео (MP4):" : "адрес потока (MP3/AAC):";
    ui_text(fb, LCD_W, LCD_H, 12, CONTENT_Y0 + 8, target, UI_SCALE_TEXT,
            UI_COLOR_URL, UI_COLOR_BG);
    const char *hint = "GO внизу или справа - начать";
    ui_text(fb, LCD_W, LCD_H, LCD_W - ui_text_width(hint, 1) - 12, CONTENT_Y0 + 8,
            hint, 1, UI_COLOR_BORDER, UI_COLOR_BG);

    draw_keyboard(fb);
}

/* Start a video (preset or custom URL): loading frame first, then hand
 * the panel over to the render on success. The caller sets videop_back. */
static void video_start_ui(const char *url)
{
    void *fb = NULL;
    app_lcd_get_fb(br.fb_idx, &fb);
    if (fb != NULL) {
        draw_loading(fb, "видео");
        app_lcd_flush(br.fb_idx);
        br.fb_idx ^= 1;
    }
    if (media_video_start(url) == ESP_OK) {
        br.state = ST_VIDEOP;       /* no UI drawing until playback stops */
    } else {
        br.state = ST_VIDEO;
        draw_screen();
    }
}

/* GO pressed on the URL-input screen (or the keyboard GO): apply the URL */
static void urlin_apply(void)
{
    char url[128];
    if (!media_url_norm(br.query, url, sizeof(url))) {
        return;
    }
    if (br.urlin_video) {
        videop_back = ST_VIDEO;
        video_start_ui(url);
    } else {
        media_radio_start(url);
        br.state = ST_RADIO;
        draw_screen();
    }
}

#endif /* CONFIG_EB_MEDIA_ENABLE */


static void draw_splash(uint16_t *fb)
{
    ui_fill_rect(fb, LCD_W, LCD_H, 0, 0, LCD_W, LCD_H, UI_COLOR_BG);

    const char *title = "CamBrowser";
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(title, UI_SCALE_TITLE * 2)) / 2,
            200, title, UI_SCALE_TITLE * 2, UI_COLOR_ACCENT, UI_COLOR_BG);

    const char *sub = "ethernet text browser on esp32-p4";
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(sub, UI_SCALE_TEXT)) / 2,
            280, sub, UI_SCALE_TEXT, UI_COLOR_URL, UI_COLOR_BG);

    char st[64];
    snprintf(st, sizeof(st), "net: %s", app_eth_status_str());
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(st, UI_SCALE_TEXT)) / 2,
            340, st, UI_SCALE_TEXT, UI_COLOR_FG, UI_COLOR_BG);

    const char *hint = app_eth_ready() ? "starting..." : "connect the ethernet cable";
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(hint, UI_SCALE_TEXT)) / 2,
            380, hint, UI_SCALE_TEXT, UI_COLOR_BORDER, UI_COLOR_BG);
}

static void draw_home(uint16_t *fb)
{
    draw_status_bar(fb);
    draw_url_row(fb);

    ui_fill_rect(fb, LCD_W, LCD_H, 0, CONTENT_Y0, LCD_W, CONTENT_Y1, UI_COLOR_BG);

#if CONFIG_EB_MEDIA_ENABLE
    for (size_t i = 0; i < HOME_APPS_N; i++) {
        ui_button(fb, LCD_W, LCD_H, &home_apps[i], false);
    }
    const char *hint = "поиск: введите запрос и нажмите GO";
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(hint, UI_SCALE_TEXT)) / 2,
            296, hint, UI_SCALE_TEXT, UI_COLOR_BORDER, UI_COLOR_BG);
#else
    const char *hint = "type a search query, then GO";
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(hint, UI_SCALE_TEXT)) / 2,
            (CONTENT_Y0 + CONTENT_Y1) / 2 - 8, hint, UI_SCALE_TEXT,
            UI_COLOR_BORDER, UI_COLOR_BG);
#endif

    draw_keyboard(fb);
}

static void draw_results(uint16_t *fb)
{
    draw_status_bar(fb);
    draw_url_row(fb);
    ui_fill_rect(fb, LCD_W, LCD_H, 0, CONTENT_Y0, LCD_W, LCD_H, UI_COLOR_BG);

    if (br.serp_error || br.result_count == 0) {
        const char *msg = br.serp_error ? "search failed - tap to go back"
                                        : "no results (antibot page?) - tap to go back";
        ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(msg, UI_SCALE_TEXT)) / 2,
                CONTENT_Y0 + 60, msg, UI_SCALE_TEXT, UI_COLOR_ERR, UI_COLOR_BG);
        return;
    }

    int first = br.page_scroll;
    int rows = (CONTENT_Y1_FULL - CONTENT_Y0) / ROW_H;
    for (int i = 0; i < rows; i++) {
        int idx = first + i;
        if (idx >= br.result_count) {
            break;
        }
        int y = CONTENT_Y0 + i * ROW_H;

        char num[16]; /* >= 13: worst-case "%2d." is 11 (int) + '.' + NUL — GCC format-truncation */
        snprintf(num, sizeof(num), "%2d.", idx + 1);
        ui_text(fb, LCD_W, LCD_H, 8, y + 8, num, UI_SCALE_TEXT, UI_COLOR_URL, UI_COLOR_BG);

        char fitted[WEB_RESULT_TITLE_MAX];
        ui_text_fit(fitted, sizeof(fitted), br.results[idx].title,
                    LCD_W - 90, UI_SCALE_TEXT);
        ui_text(fb, LCD_W, LCD_H, 56, y + 8, fitted, UI_SCALE_TEXT,
                UI_COLOR_FG, UI_COLOR_BG);

        ui_fill_rect(fb, LCD_W, LCD_H, 8, y + ROW_H - 1, LCD_W - 8, y + ROW_H,
                     UI_COLOR_BAR);
    }

    draw_nav_bar(fb);
}

static void draw_page(uint16_t *fb)
{
    draw_status_bar(fb);
    draw_url_row(fb);
    ui_fill_rect(fb, LCD_W, LCD_H, 0, CONTENT_Y0, LCD_W, LCD_H, UI_COLOR_BG);

    if (br.page_text == NULL || br.line_count == 0) {
        const char *msg = br.page_text == NULL ? "page load failed - tap for back"
                                               : "empty page - tap for back";
        ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(msg, UI_SCALE_TEXT)) / 2,
                CONTENT_Y0 + 60, msg, UI_SCALE_TEXT, UI_COLOR_ERR, UI_COLOR_BG);
        return;
    }

    int rows = (CONTENT_Y1_FULL - CONTENT_Y0) / LINE_H;
    for (int i = 0; i < rows; i++) {
        int li = br.page_scroll + i;
        if (li >= br.line_count) {
            break;
        }
        char buf[128];
        int len = br.lines[li].len;
        if (len > (int)sizeof(buf) - 1) {
            len = (int)sizeof(buf) - 1;
        }
        memcpy(buf, br.lines[li].s, len);
        buf[len] = '\0';
        /* never cut a UTF-8 sequence at the buffer boundary */
        while (len > 0 && (((unsigned char)buf[len]) & 0xC0) == 0x80) {
            buf[--len] = '\0';
        }
        ui_text(fb, LCD_W, LCD_H, 12, CONTENT_Y0 + i * LINE_H, buf,
                UI_SCALE_TEXT, UI_COLOR_FG, UI_COLOR_BG);
    }

    draw_nav_bar(fb);
}

static void draw_loading(uint16_t *fb, const char *what)
{
    ui_fill_rect(fb, LCD_W, LCD_H, 0, 0, LCD_W, LCD_H, UI_COLOR_BG);
    draw_status_bar(fb);

    static int dots = 0;
    char msg[64];
    dots = (dots + 1) % 4;
    snprintf(msg, sizeof(msg), "%s%s", what, "..." + (3 - dots));

    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(msg, UI_SCALE_TITLE)) / 2,
            280, msg, UI_SCALE_TITLE, UI_COLOR_ACCENT, UI_COLOR_BG);
}

static void draw_screen(void)
{
#if CONFIG_EB_MEDIA_ENABLE
    /* hard guard: while a video is playing the esp_video_render LCD backend
     * owns the DPI frame buffers - any UI write/flush here would corrupt
     * the playback (or crash on a torn double-buffer flip) */
    if (br.state == ST_VIDEOP && media_video_active()) {
        return;
    }
#endif

    void *fb = NULL;
    app_lcd_get_fb(br.fb_idx, &fb);
    if (fb == NULL) {
        return;
    }

    switch (br.state) {
    case ST_SPLASH:   draw_splash(fb);       break;
    case ST_HOME:     draw_home(fb);         break;
    case ST_LOADING:  draw_loading(fb, "searching");  break;
    case ST_RESULTS:  draw_results(fb);      break;
    case ST_PAGELOAD: draw_loading(fb, "loading");    break;
    case ST_PAGE:     draw_page(fb);         break;
#if CONFIG_EB_MEDIA_ENABLE
    case ST_RADIO:    draw_radio(fb);        break;
    case ST_VIDEO:    draw_video(fb);        break;
    case ST_URLIN:    draw_urlin(fb);        break;
    case ST_FILES:    draw_files(fb);        break;
    case ST_VIDEOP:   /* render owns the panel - nothing to draw */ break;
#endif
    }

    app_lcd_flush(br.fb_idx);
    br.fb_idx ^= 1;
}

/* --- Actions -------------------------------------------------------------------- */

static void do_search(void)
{
    if (br.query[0] == '\0') {
        return;
    }
    br.state = ST_LOADING;
    draw_screen();

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = web_search(br.query, br.engine_google, &body, &len);

    br.result_count = 0;
    br.serp_error = true;
    if (err == ESP_OK && body != NULL) {
        int n = br.engine_google ?
                html_parse_serp_google(body, br.results, WEB_RESULTS_MAX) :
                html_parse_serp_ddg(body, br.results, WEB_RESULTS_MAX);
        if (n > 0) {
            br.result_count = n;
            br.serp_error = false;
        }
        free(body);
    } else {
        ESP_LOGW(TAG, "search failed: %s", esp_err_to_name(err));
    }

    br.state = ST_RESULTS;
    draw_screen();
}

static void open_result(int idx)
{
    if (idx < 0 || idx >= br.result_count) {
        return;
    }

    br.state = ST_PAGELOAD;
    draw_screen();

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = web_get(br.results[idx].url, NULL, &body, &len, NULL);

    page_free();
    if (err == ESP_OK && body != NULL) {
        char *text = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
        if (text != NULL) {
            html_to_text(body, text, len + 1);
            br.page_text = text;
            page_wrap();
        }
        free(body);
    } else {
        ESP_LOGW(TAG, "page fetch failed: %s", esp_err_to_name(err));
    }

    br.state = ST_PAGE;
    draw_screen();
}

/* Append one key codepoint (ASCII or Cyrillic) as UTF-8 */
static void query_append_cp(uint32_t cp)
{
    char utf8[3];
    int n = kb_key_label(cp, utf8);
    size_t len = strlen(br.query);
    if (len + n < sizeof(br.query)) {
        memcpy(br.query + len, utf8, n);
        br.query[len + n] = '\0';
    }
}

/* --- Main loop ------------------------------------------------------------------- */

static void browser_task(void *arg)
{
    /* Standalone boot: the LCD is already initialized, the browser owns the
     * screen from the first frame - no camera to stop, draw immediately */
    br.state = ST_SPLASH;
    draw_screen();

    touch_point_t pts[TOUCH_MAX_POINTS];
    bool prev_touch = false;
    int64_t splash_start = esp_timer_get_time();

    while (1) {
        int n = touch_poll(pts, TOUCH_MAX_POINTS);
        bool touch_now = (n > 0);
        int tap_x = (n > 0) ? pts[0].x : 0;
        int tap_y = (n > 0) ? pts[0].y : 0;
        bool tap = touch_now && !prev_touch;    /* press-and-release edge */

        switch (br.state) {
        case ST_SPLASH:
            if (app_eth_ready()) {
                br.state = ST_HOME;
                draw_screen();
            } else if (esp_timer_get_time() - splash_start > 1000000LL) {
                splash_start = esp_timer_get_time();
                draw_screen();  /* refresh the status line */
            } else if (tap && !app_eth_ready()) {
                /* tap during splash = retry immediately (no-op until net) */
                splash_start = 0;
            }
            break;

        case ST_HOME:
            if (tap) {
                int id = BTN_NONE;
                if (urlrow_go_hit(tap_x, tap_y) && br.query[0] != '\0') {
                    id = BTN_GO;
                }
#if CONFIG_EB_MEDIA_ENABLE
                else if (home_apps_hit(tap_x, tap_y, &id)) {
                    /* id filled by the helper */
                }
#endif
                else {
                    id = keyboard_hit(tap_x, tap_y);
                }
                if (id >= BTN_CHAR) {
                    query_append_cp((uint32_t)id);
                    draw_screen();
                } else {
                    switch (id) {
#if CONFIG_EB_MEDIA_ENABLE
                    case BTN_RADIO:
                        br.state = ST_RADIO;
                        draw_screen();
                        break;
                    case BTN_VIDEO:
                        br.state = ST_VIDEO;
                        draw_screen();
                        break;
#endif
                    case BTN_KBD:
                        br.kb_ru = !br.kb_ru;
                        draw_screen();
                        break;
                    case BTN_DEL: {
                        /* strip one whole UTF-8 codepoint, not one byte */
                        size_t len = strlen(br.query);
                        while (len > 0 &&
                               (((unsigned char)br.query[len - 1]) & 0xC0) == 0x80) {
                            len--;
                        }
                        if (len > 0) {
                            br.query[len - 1] = '\0';
                        }
                        draw_screen();
                        break;
                    }
                    case BTN_SPC:
                        query_append_cp(' ');
                        draw_screen();
                        break;
                    case BTN_ENGINE:
                        br.engine_google = !br.engine_google;
                        draw_screen();
                        break;
                    case BTN_GO:
                        do_search();
                        break;
                    default:
                        break;
                    }
                }
            }
            break;

        case ST_RESULTS:
            if (tap) {
                int nav_id = 0;
                if (nav_hit(tap_x, tap_y, &nav_id)) {
                    if (nav_id == BTN_HOME) {
                        br.state = ST_HOME;
                        draw_screen();
                    } else if (nav_id == BTN_UP && br.page_scroll > 0) {
                        br.page_scroll--;
                        draw_screen();
                    } else if (nav_id == BTN_DOWN &&
                               br.page_scroll + (CONTENT_Y1_FULL - CONTENT_Y0) / ROW_H <
                               br.result_count) {
                        br.page_scroll++;
                        draw_screen();
                    }
                } else if (tap_y >= CONTENT_Y0 && tap_y < CONTENT_Y1_FULL) {
                    int row = (tap_y - CONTENT_Y0) / ROW_H;
                    int idx = br.page_scroll + row;
                    if (br.serp_error || idx >= br.result_count) {
                        br.state = ST_HOME;
                        draw_screen();
                    } else {
                        open_result(idx);
                    }
                }
            }
            break;

        case ST_PAGE:
            if (tap) {
                int nav_id = 0;
                if (nav_hit(tap_x, tap_y, &nav_id)) {
                    if (nav_id == BTN_HOME) {
                        page_free();
                        br.state = ST_HOME;
                        draw_screen();
                    } else if (nav_id == BTN_BACK) {
                        page_free();
                        br.state = ST_RESULTS;
                        draw_screen();
                    } else if (nav_id == BTN_UP && br.page_scroll > 0) {
                        br.page_scroll--;
                        draw_screen();
                    } else if (nav_id == BTN_DOWN &&
                               br.page_scroll + (CONTENT_Y1_FULL - CONTENT_Y0) / LINE_H <
                               br.line_count) {
                        br.page_scroll++;
                        draw_screen();
                    }
                } else if (tap_y >= CONTENT_Y0 && tap_y < CONTENT_Y1_FULL) {
                    /* page body tap = scroll down */
                    br.page_scroll += 4;
                    draw_screen();
                }
            }
            break;

#if CONFIG_EB_MEDIA_ENABLE
        case ST_RADIO:
            if (tap) {
                int id = radio_hit(tap_x, tap_y);
                if (id >= BTN_STATION && id < (int)(BTN_STATION + RADIO_STATIONS_N)) {
                    media_radio_start(radio_stations[id - BTN_STATION].url);
                    draw_screen();
                } else {
                    switch (id) {
                    case BTN_MPAUSE:
                        if (media_get_state() == MEDIA_STATE_PAUSED) {
                            media_resume();
                        } else {
                            media_pause();
                        }
                        draw_screen();
                        break;
                    case BTN_MSTOP:
                        media_stop();
                        draw_screen();
                        break;
                    case BTN_VOLDN:
                        media_set_volume(media_get_volume() - 10);
                        draw_screen();
                        break;
                    case BTN_VOLUP:
                        media_set_volume(media_get_volume() + 10);
                        draw_screen();
                        break;
                    case BTN_MURL:
                        br.urlin_video = false;
                        br.query[0] = '\0';
                        br.state = ST_URLIN;
                        draw_screen();
                        break;
                    case BTN_SD:
                        sd_from_video = false;
                        sd_scan();
                        br.state = ST_FILES;
                        draw_screen();
                        break;
                    case BTN_MBACK:
                        br.state = ST_HOME;
                        draw_screen();
                        break;
                    default:
                        break;
                    }
                }
            }
            break;

        case ST_VIDEO:
            if (tap) {
                int id = video_hit(tap_x, tap_y);
                if (id >= BTN_VIDPRESET && id < (int)(BTN_VIDPRESET + VIDEO_PRESETS_N)) {
                    videop_back = ST_VIDEO;
                    video_start_ui(video_presets[id - BTN_VIDPRESET].url);
                } else {
                    switch (id) {
                    case BTN_MURL:
                        br.urlin_video = true;
                        br.query[0] = '\0';
                        br.state = ST_URLIN;
                        draw_screen();
                        break;
                    case BTN_SD:
                        sd_from_video = true;
                        sd_scan();
                        br.state = ST_FILES;
                        draw_screen();
                        break;
                    case BTN_MBACK:
                        br.state = ST_HOME;
                        draw_screen();
                        break;
                    default:
                        break;
                    }
                }
            }
            break;

        case ST_FILES:
            if (tap) {
                int id = files_hit(tap_x, tap_y);
                if (id == BTN_FILEROW) {
                    int row = (tap_y - SD_ROWS_Y0) / SD_ROW_STRIDE;
                    sd_play(sd_scroll + row);
                } else {
                    switch (id) {
                    case BTN_FREFR:
                        sdcard_reprobe();
                        sd_scan();
                        draw_screen();
                        break;
                    case BTN_FUP:
                        if (sd_scroll > 0) {
                            sd_scroll--;
                            draw_screen();
                        }
                        break;
                    case BTN_FDOWN:
                        if (sd_scroll + SD_ROWS < sd_files_n) {
                            sd_scroll++;
                            draw_screen();
                        }
                        break;
                    case BTN_MBACK:
                        br.state = sd_from_video ? ST_VIDEO : ST_RADIO;
                        draw_screen();
                        break;
                    default:
                        break;
                    }
                }
            }
            break;

        case ST_URLIN:
            if (tap) {
                if (urlrow_go_hit(tap_x, tap_y) && br.query[0] != '\0') {
                    urlin_apply();
                } else {
                    int id = keyboard_hit(tap_x, tap_y);
                    if (id >= BTN_CHAR) {
                        query_append_cp((uint32_t)id);
                        draw_screen();
                    } else {
                        switch (id) {
                        case BTN_ENGINE:    /* НАЗАД on this screen */
                            br.state = br.urlin_video ? ST_VIDEO : ST_RADIO;
                            draw_screen();
                            break;
                        case BTN_KBD:
                            br.kb_ru = !br.kb_ru;
                            draw_screen();
                            break;
                        case BTN_DEL: {
                            size_t len = strlen(br.query);
                            while (len > 0 &&
                                   (((unsigned char)br.query[len - 1]) & 0xC0) == 0x80) {
                                len--;
                            }
                            if (len > 0) {
                                br.query[len - 1] = '\0';
                            }
                            draw_screen();
                            break;
                        }
                        case BTN_SPC:
                            query_append_cp(' ');
                            draw_screen();
                            break;
                        case BTN_GO:
                            urlin_apply();
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
            break;

        case ST_VIDEOP:
            /* the render owns the panel: no UI drawing in this state */
            if (media_get_state() == MEDIA_STATE_FINISHED ||
                media_get_state() == MEDIA_STATE_ERROR ||
                media_video_active() == false) {
                media_stop();
                br.state = videop_back;
                draw_screen();
            } else if (tap) {
                media_stop();
                br.state = videop_back;
                draw_screen();
            }
            break;
#endif

        default:
            break;
        }

        prev_touch = touch_now;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void browser_start(void)
{
    memset(&br, 0, sizeof(br));
    br.fb_idx = 1;  /* draw on the second fb, flush the first via app_lcd_flush */
#if CONFIG_EB_ENGINE_GOOGLE
    br.engine_google = true;
#endif

    esp_err_t eth_err = app_eth_start();
    if (eth_err != ESP_OK) {
        ESP_LOGE(TAG, "eth start failed: %s", esp_err_to_name(eth_err));
    }
    esp_err_t tp_err = touch_init();
    if (tp_err != ESP_OK) {
        ESP_LOGW(TAG, "touch not found (%s) - browser will be view-only",
                 esp_err_to_name(tp_err));
    }

    if (xTaskCreatePinnedToCore(browser_task, "cambrowser", 16 * 1024, NULL,
                                3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create browser task");
    }
}

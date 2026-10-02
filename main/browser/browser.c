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

static const char *TAG = "browser";

/* --- Layout -------------------------------------------------------------- */

#define LCD_W           EXAMPLE_LCD_H_RES
#define LCD_H           EXAMPLE_LCD_V_RES

#define BAR_H           32      /* top status bar          */
#define URL_Y           (BAR_H) /* query row               */
#define URL_H           44
#define GO_W            96

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

/* Button ids */
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
    BTN_CHAR = 0x20,        /* ids >= BTN_CHAR are literal chars */
};

typedef enum {
    ST_SPLASH,
    ST_HOME,
    ST_LOADING,
    ST_RESULTS,
    ST_PAGELOAD,
    ST_PAGE,
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

    ui_text(fb, LCD_W, LCD_H, 8, (BAR_H - UI_FONT_H) / 2,
            left, 1, UI_COLOR_OK, UI_COLOR_BAR);
    ui_text(fb, LCD_W, LCD_H, LCD_W - ui_text_width(right, 1) - 8,
            (BAR_H - UI_FONT_H) / 2, right, 1, UI_COLOR_URL, UI_COLOR_BAR);
}

static void draw_url_row(uint16_t *fb)
{
    static const ui_button_t go = {
        .x = LCD_W - GO_W - 8, .y = URL_Y + 2, .w = GO_W, .h = URL_H - 4,
        .label = "GO", .id = BTN_GO
    };

    ui_fill_rect(fb, LCD_W, LCD_H, 0, URL_Y, LCD_W, URL_Y + URL_H, UI_COLOR_BG);
    ui_frame(fb, LCD_W, LCD_H, 4, URL_Y + 2, LCD_W - GO_W - 12, URL_Y + URL_H - 2,
             2, UI_COLOR_BORDER);

    char fitted[96];
    ui_text_fit(fitted, sizeof(fitted), br.query, LCD_W - GO_W - 40, UI_SCALE_TEXT);
    ui_text(fb, LCD_W, LCD_H, 10, URL_Y + (URL_H - UI_FONT_H * UI_SCALE_TEXT) / 2,
            fitted, UI_SCALE_TEXT, UI_COLOR_FG, UI_COLOR_BG);

    /* caret */
    int cx = 10 + ui_text_width(fitted, UI_SCALE_TEXT) + 2;
    if ((esp_timer_get_time() / 500000) & 1) {
        ui_fill_rect(fb, LCD_W, LCD_H, cx, URL_Y + 12, cx + UI_SCALE_TEXT,
                     URL_Y + URL_H - 12, UI_COLOR_ACCENT);
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
    return kb_bottom[i].id == BTN_KBD ? (br.kb_ru ? "RU" : "EN")
                                      : (br.engine_google ? "GGL" : "DDG");
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
    const char *hint = "type a search query, then GO";
    ui_text(fb, LCD_W, LCD_H, (LCD_W - ui_text_width(hint, UI_SCALE_TEXT)) / 2,
            (CONTENT_Y0 + CONTENT_Y1) / 2 - 8, hint, UI_SCALE_TEXT,
            UI_COLOR_BORDER, UI_COLOR_BG);

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
                int id = (urlrow_go_hit(tap_x, tap_y) && br.query[0] != '\0')
                         ? BTN_GO : keyboard_hit(tap_x, tap_y);
                if (id >= BTN_CHAR) {
                    query_append_cp((uint32_t)id);
                    draw_screen();
                } else {
                    switch (id) {
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

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
#include "app_video.h"
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
#define CONTENT_Y1      392     /* keyboard starts below   */
#define CONTENT_Y1_FULL (LCD_H - 64) /* without keyboard   */

#define ROW_H           36      /* results list row        */
#define LINE_H          20      /* page text line          */
#define PAGE_COLS       ((LCD_W - 24) / (UI_FONT_W * UI_SCALE_TEXT))

/* Keyboard geometry: 5 rows, keys 64x36, gap 8, centered */
#define KEY_W           64
#define KEY_H           36
#define KEY_GAP         8

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
    int video_fd;
    int fb_idx;
    browser_state_t state;

    char query[96];
    bool engine_google;

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
            p++;
            col++;
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

static const char *KB_ROWS[] = {
    "1234567890-",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm_/",
};
#define KB_ROWS_NUM 4

static void draw_keyboard(uint16_t *fb)
{
    ui_fill_rect(fb, LCD_W, LCD_H, 0, CONTENT_Y1, LCD_W, LCD_H, 0x0000);

    int y = CONTENT_Y1 + 4;
    for (int r = 0; r < KB_ROWS_NUM; r++) {
        const char *row = KB_ROWS[r];
        int row_w = (int)strlen(row) * KEY_W + ((int)strlen(row) - 1) * KEY_GAP;
        int x = (LCD_W - row_w) / 2;

        for (const char *c = row; *c != '\0'; c++, x += KEY_W + KEY_GAP) {
            ui_button_t key = { .x = x, .y = y, .w = KEY_W, .h = KEY_H,
                                .label = NULL, .id = (int)*c };
            char lab[2] = { *c, 0 };
            key.label = lab;
            ui_button(fb, LCD_W, LCD_H, &key, false);
        }
        y += KEY_H + 4;
    }

    /* bottom row: SPC / DEL / engine / GO */
    struct { int w; const char *label; int id; } bottom[] = {
        { 160, "SPC",   BTN_SPC },
        { 96,  "DEL",   BTN_DEL },
        { 128, br.engine_google ? "GGL" : "DDG", BTN_ENGINE },
        { 128, "GO",    BTN_GO },
    };
    int total = 0;
    for (size_t i = 0; i < 4; i++) {
        total += bottom[i].w;
    }
    total += 3 * KEY_GAP;

    int x = (LCD_W - total) / 2;
    for (size_t i = 0; i < 4; i++) {
        ui_button_t key = { .x = x, .y = y, .w = bottom[i].w, .h = KEY_H,
                            .label = bottom[i].label, .id = bottom[i].id };
        ui_button(fb, LCD_W, LCD_H, &key, false);
        x += bottom[i].w + KEY_GAP;
    }
}

/* Returns BTN_* id or 0 when the tap hit nothing */
static int keyboard_hit(int x, int y)
{
    if (y < CONTENT_Y1) {
        return BTN_NONE;
    }

    int ky = CONTENT_Y1 + 4;
    for (int r = 0; r < KB_ROWS_NUM; r++) {
        const char *row = KB_ROWS[r];
        int row_w = (int)strlen(row) * KEY_W + ((int)strlen(row) - 1) * KEY_GAP;
        int kx = (LCD_W - row_w) / 2;

        if (y >= ky && y < ky + KEY_H) {
            for (const char *c = row; *c != '\0'; c++, kx += KEY_W + KEY_GAP) {
                if (x >= kx && x < kx + KEY_W) {
                    return (int)*c;
                }
            }
            return BTN_NONE;
        }
        ky += KEY_H + 4;
    }

    struct { int w; const char *label; int id; } bottom[] = {
        { 160, "SPC",   BTN_SPC },
        { 96,  "DEL",   BTN_DEL },
        { 128, br.engine_google ? "GGL" : "DDG", BTN_ENGINE },
        { 128, "GO",    BTN_GO },
    };
    int total = 0;
    for (size_t i = 0; i < 4; i++) {
        total += bottom[i].w;
    }
    total += 3 * KEY_GAP;
    int x0 = (LCD_W - total) / 2;

    if (y >= ky && y < ky + KEY_H) {
        for (size_t i = 0; i < 4; i++) {
            if (x >= x0 && x < x0 + bottom[i].w) {
                return bottom[i].id;
            }
            x0 += bottom[i].w + KEY_GAP;
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

static void query_append(char c)
{
    size_t len = strlen(br.query);
    if (len + 1 < sizeof(br.query)) {
        br.query[len] = c;
        br.query[len + 1] = '\0';
    }
}

/* --- Main loop ------------------------------------------------------------------- */

static void browser_task(void *arg)
{
    /* Give the camera preview a moment, then take the screen over */
    vTaskDelay(pdMS_TO_TICKS(1000));
    app_video_stream_task_stop(br.video_fd);
    vTaskDelay(pdMS_TO_TICKS(600));

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
                    query_append((char)id);
                    draw_screen();
                } else {
                    switch (id) {
                    case BTN_DEL: {
                        size_t len = strlen(br.query);
                        if (len > 0) {
                            br.query[len - 1] = '\0';
                        }
                        draw_screen();
                        break;
                    }
                    case BTN_SPC:
                        query_append(' ');
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

void browser_start(int video_fd)
{
    memset(&br, 0, sizeof(br));
    br.video_fd = video_fd;
    br.fb_idx = 1;  /* camera used fb[0..n] round-robin; start on the second */
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

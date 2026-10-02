/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser UI primitives for RGB565 frame buffers.
 *
 * Thin drawing layer on top of the shared 8x8 font (app_overlay_font8x8):
 * rounded cards, clipped text, flat icons, progress bars and buttons.
 * All functions are silent no-ops on out-of-range arguments; every writer
 * clips to fb_w/fb_h so a bad layout can never write past the end of the
 * DPI frame buffer (the class of bug fixed in commit 68da2b8 for the PPA
 * path).
 *
 * Theme v2 "friendly dark": deep navy background, panel cards, one cyan
 * accent. Designed after the manufacturer's esp-brookesia phone demo
 * (status bar + rounded app cards) but drawn with plain RGB565 spans so
 * it costs no extra RAM.
 */

#include <stdio.h>
#include <string.h>
#include "app_overlay.h"
#include "camos/ui.h"
#include "camos/ui_icons.h"

/* Decode one UTF-8 codepoint at p (NUL-safe). Advances *pp past the whole
 * sequence. Returns the codepoint, or -1 for an invalid/overlong sequence
 * (caller draws '?'). Only ASCII (1 byte) and Cyrillic-range 2-byte
 * sequences matter for the current font; 3/4-byte sequences are skipped
 * whole so a single char becomes exactly one '?'. */
static int ui_utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)(*pp);
    unsigned char c = p[0];

    if (c < 0x80) {
        (*pp)++;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        int cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        *pp += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *pp += 3;
        return -1;
    }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *pp += 4;
        return -1;
    }
    (*pp)++;
    return -1;
}

void ui_fill_rect(uint16_t *fb, int fb_w, int fb_h,
                  int x0, int y0, int x1, int y1, uint16_t color)
{
    if (fb == NULL) {
        return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    for (int y = y0; y < y1; y++) {
        uint16_t *row = fb + (size_t)y * fb_w + x0;
        for (int x = x0; x < x1; x++) {
            *row++ = color;
        }
    }
}

void ui_frame(uint16_t *fb, int fb_w, int fb_h,
              int x0, int y0, int x1, int y1, int thick, uint16_t color)
{
    ui_fill_rect(fb, fb_w, fb_h, x0, y0, x1, y0 + thick, color);          /* top */
    ui_fill_rect(fb, fb_w, fb_h, x0, y1 - thick, x1, y1, color);          /* bottom */
    ui_fill_rect(fb, fb_w, fb_h, x0, y0, x0 + thick, y1, color);          /* left */
    ui_fill_rect(fb, fb_w, fb_h, x1 - thick, y0, x1, y1, color);          /* right */
}

/* Integer sqrt good enough for corner insets (r <= 32) */
static int ui_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) {
        r++;
    }
    return r;
}

void ui_rrect(uint16_t *fb, int fb_w, int fb_h,
              int x0, int y0, int x1, int y1, int r, uint16_t color)
{
    if (fb == NULL || x1 <= x0 || y1 <= y0) {
        return;
    }
    int h = y1 - y0, w = x1 - x0;
    if (r > h / 2) {
        r = h / 2;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r <= 0) {
        ui_fill_rect(fb, fb_w, fb_h, x0, y0, x1, y1, color);
        return;
    }

    /* middle band: full width */
    ui_fill_rect(fb, fb_w, fb_h, x0, y0 + r, x1, y1 - r, color);
    /* top/bottom strips between the corner arcs */
    ui_fill_rect(fb, fb_w, fb_h, x0 + r, y0, x1 - r, y0 + r, color);
    ui_fill_rect(fb, fb_w, fb_h, x0 + r, y1 - r, x1 - r, y1, color);

    /* corners: per-row inset from the circle equation */
    for (int dy = 0; dy < r; dy++) {
        int ry = r - 1 - dy;                 /* 0 at the outermost row */
        int dx = r - ui_isqrt(r * r - ry * ry);
        int y_top = y0 + dy;
        int y_bot = y1 - 1 - dy;
        /* top-left + top-right */
        ui_fill_rect(fb, fb_w, fb_h, x0 + dx, y_top, x0 + r, y_top + 1, color);
        ui_fill_rect(fb, fb_w, fb_h, x1 - r, y_top, x1 - dx, y_top + 1, color);
        /* bottom-left + bottom-right */
        ui_fill_rect(fb, fb_w, fb_h, x0 + dx, y_bot, x0 + r, y_bot + 1, color);
        ui_fill_rect(fb, fb_w, fb_h, x1 - r, y_bot, x1 - dx, y_bot + 1, color);
    }
}

void ui_rrect_frame(uint16_t *fb, int fb_w, int fb_h,
                    int x0, int y0, int x1, int y1, int r, uint16_t color)
{
    if (fb == NULL || x1 <= x0 || y1 <= y0) {
        return;
    }
    int h = y1 - y0, w = x1 - x0;
    if (r > h / 2) {
        r = h / 2;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r <= 0) {
        ui_frame(fb, fb_w, fb_h, x0, y0, x1, y1, 1, color);
        return;
    }

    /* straight edges (1 px) */
    ui_fill_rect(fb, fb_w, fb_h, x0 + r, y0, x1 - r, y0 + 1, color);
    ui_fill_rect(fb, fb_w, fb_h, x0 + r, y1 - 1, x1 - r, y1, color);
    ui_fill_rect(fb, fb_w, fb_h, x0, y0 + r, x0 + 1, y1 - r, color);
    ui_fill_rect(fb, fb_w, fb_h, x1 - 1, y0 + r, x1, y1 - r, color);

    /* arcs: one pixel per row at the circle boundary */
    for (int dy = 0; dy < r; dy++) {
        int ry = r - 1 - dy;
        int dx = r - ui_isqrt(r * r - ry * ry);
        int y_top = y0 + dy;
        int y_bot = y1 - 1 - dy;
        ui_fill_rect(fb, fb_w, fb_h, x0 + dx, y_top, x0 + dx + 1, y_top + 1, color);
        ui_fill_rect(fb, fb_w, fb_h, x1 - dx - 1, y_top, x1 - dx, y_top + 1, color);
        ui_fill_rect(fb, fb_w, fb_h, x0 + dx, y_bot, x0 + dx + 1, y_bot + 1, color);
        ui_fill_rect(fb, fb_w, fb_h, x1 - dx - 1, y_bot, x1 - dx, y_bot + 1, color);
    }
}

void ui_circle(uint16_t *fb, int fb_w, int fb_h,
               int cx, int cy, int r, uint16_t color)
{
    if (fb == NULL || r <= 0) {
        return;
    }
    for (int dy = -r; dy <= r; dy++) {
        int dx = ui_isqrt(r * r - dy * dy);
        ui_fill_rect(fb, fb_w, fb_h, cx - dx, cy + dy, cx + dx + 1, cy + dy + 1, color);
    }
}

void ui_card(uint16_t *fb, int fb_w, int fb_h,
             int x0, int y0, int x1, int y1, bool pressed)
{
    ui_rrect(fb, fb_w, fb_h, x0, y0, x1, y1, UI_R_CARD,
             pressed ? UI_COLOR_BG_PRESS : UI_COLOR_PANEL);
    ui_rrect_frame(fb, fb_w, fb_h, x0, y0, x1, y1, UI_R_CARD,
                   pressed ? UI_COLOR_ACCENT : UI_COLOR_BORDER);
}

int ui_text_width(const char *s, int scale)
{
    if (s == NULL) {
        return 0;
    }
    int n = 0;
    for (const char *p = s; *p != '\0'; ) {
        ui_utf8_next(&p);
        n++;
    }
    return n * UI_FONT_W * scale;
}

int ui_text(uint16_t *fb, int fb_w, int fb_h,
            int x, int y, const char *s, int scale,
            uint16_t fg, uint16_t bg)
{
    if (fb == NULL || s == NULL || scale <= 0) {
        return x;
    }

    int char_step = UI_FONT_W * scale;
    int glyph_h = UI_FONT_H * scale;

    for (const char *p = s; *p != '\0'; x += char_step) {
        if (x + char_step <= 0 || x >= fb_w || y + glyph_h <= 0 || y >= fb_h) {
            continue; /* whole glyph outside the canvas */
        }

        int cp = ui_utf8_next(&p);
        const uint8_t *glyph;
        if (cp >= 0x20 && cp <= 0x7E) {
            glyph = app_overlay_font8x8[cp - 0x20];
        } else if (cp >= 0x0410 && cp <= 0x044F) {
            glyph = app_overlay_font8x8_cyr[cp - 0x0410];   /* А..Я, а..я */
        } else if (cp == 0x0401) {
            glyph = app_overlay_font8x8_cyr[64];            /* Ё */
        } else if (cp == 0x0451) {
            glyph = app_overlay_font8x8_cyr[65];            /* ё */
        } else {
            glyph = app_overlay_font8x8['?' - 0x20];
        }

        for (int row = 0; row < UI_FONT_H; row++) {
            int py = y + row * scale;
            for (int col = 0; col < UI_FONT_W; col++) {
                uint16_t color = (glyph[row] >> col) & 0x1 ? fg : bg;
                int px = x + col * scale;
                ui_fill_rect(fb, fb_w, fb_h, px, py, px + scale, py + scale, color);
            }
        }
    }
    return x;
}

int ui_text_fit(char *out, size_t out_size, const char *s, int max_px, int scale)
{
    if (out == NULL || out_size == 0 || s == NULL) {
        return 0;
    }

    int step = UI_FONT_W * scale;
    int max_chars = (int)(out_size - 1);
    if (max_chars > max_px / step) {
        max_chars = max_px / step;
    }

    /* Byte length of the first max_chars UTF-8 codepoints */
    int len = 0, chars = 0;
    while (s[len] != '\0' && chars < max_chars && len + 4 < (int)out_size) {
        const char *q = s + len;
        ui_utf8_next(&q);
        len = (int)(q - s);
        chars++;
    }
    if (s[len] == '\0') {
        memcpy(out, s, len);
        out[len] = '\0';
        return chars;
    }
    if (max_chars < 3 || len + 3 > (int)out_size) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    strcat(out, "..");
    return chars;
}

void ui_box_text(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, const char *s, int scale,
                 uint16_t fg, uint16_t bg, uint16_t box, int pad)
{
    int w = ui_text_width(s, scale);
    ui_fill_rect(fb, fb_w, fb_h, x - pad, y - pad,
                 x + w + pad, y + UI_FONT_H * scale + pad, box);
    ui_text(fb, fb_w, fb_h, x, y, s, scale, fg, bg);
}

void ui_icon(const struct ui_icon *ic, uint16_t *fb, int fb_w, int fb_h,
             int x, int y, int scale, uint16_t fg)
{
    if (fb == NULL || ic == NULL || scale <= 0) {
        return;
    }
    for (int row = 0; row < UI_ICON_H; row++) {
        uint16_t bits = ic->row[row];
        int py = y + row * scale;
        int col = 0;
        while (col < UI_ICON_W) {
            if (!((bits >> (15 - col)) & 0x1)) {
                col++;
                continue;
            }
            int run = 0;
            while (col + run < UI_ICON_W && ((bits >> (15 - col - run)) & 0x1)) {
                run++;
            }
            ui_fill_rect(fb, fb_w, fb_h, x + col * scale, py,
                         x + (col + run) * scale, py + scale, fg);
            col += run;
        }
    }
}

void ui_progress(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, int w, int h, float frac, uint16_t color)
{
    if (fb == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (frac < 0.0f) {
        frac = 0.0f;
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }
    int r = h / 2;
    ui_rrect(fb, fb_w, fb_h, x, y, x + w, y + h, r, UI_COLOR_KEY);
    ui_rrect_frame(fb, fb_w, fb_h, x, y, x + w, y + h, r, UI_COLOR_BORDER);
    int fw = (int)(w * frac);
    if (fw > h) {
        ui_rrect(fb, fb_w, fb_h, x, y, x + fw, y + h, r, color);
    } else if (fw > 0) {
        ui_fill_rect(fb, fb_w, fb_h, x, y, x + fw, y + h, color);
    }
}

void ui_spinner(uint16_t *fb, int fb_w, int fb_h,
                int cx, int cy, int r, int phase, uint16_t color)
{
    if (fb == NULL || r <= 0) {
        return;
    }
    static const int sin8[8] = { 0, 5, 7, 7, 7, 5, 0, -5 };  /* cos/sin x8, 45° steps */
    for (int i = 0; i < 3; i++) {
        int a = (phase + i * 3) & 7;
        int dx = sin8[a] * r / 8;
        int dy = sin8[(a + 2) & 7] * r / 8;
        ui_circle(fb, fb_w, fb_h, cx + dx, cy + dy, 2 + (i == 0 ? 1 : 0), color);
    }
}

void ui_button(uint16_t *fb, int fb_w, int fb_h,
               const ui_button_t *b, bool pressed)
{
    uint16_t bg  = pressed ? UI_COLOR_BG_PRESS : UI_COLOR_KEY;
    uint16_t txt = pressed ? UI_COLOR_ACCENT  : UI_COLOR_FG;

    int r = (b->h >= 40) ? UI_R_KEY : 6;
    ui_rrect(fb, fb_w, fb_h, b->x, b->y, b->x + b->w, b->y + b->h, r, bg);
    ui_rrect_frame(fb, fb_w, fb_h, b->x, b->y, b->x + b->w, b->y + b->h, r,
                   pressed ? UI_COLOR_ACCENT : UI_COLOR_BORDER);

    int sc = (b->scale > 0) ? b->scale : UI_SCALE_BTN;

    char fitted[24];
    ui_text_fit(fitted, sizeof(fitted), b->label, b->w - 4, sc);
    int text_w = ui_text_width(fitted, sc);
    int tx = b->x + (b->w - text_w) / 2;
    int ty = b->y + (b->h - UI_FONT_H * sc) / 2;
    ui_text(fb, fb_w, fb_h, tx, ty, fitted, sc, txt, bg);
}

bool ui_button_hit(const ui_button_t *b, int x, int y)
{
    return (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h);
}

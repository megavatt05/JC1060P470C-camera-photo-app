/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser UI primitives for RGB565 frame buffers.
 *
 * Thin drawing layer on top of the shared 8x8 font (app_overlay_font8x8):
 * filled rectangles, clipped text, boxes and frames. All functions are
 * silent no-ops on out-of-range arguments; every writer clips to fb_w/fb_h
 * so a bad layout can never write past the end of the DPI frame buffer
 * (the class of bug fixed in commit 68da2b8 for the PPA path).
 */

#include <stdio.h>
#include <string.h>
#include "app_overlay.h"
#include "camos/ui.h"

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

int ui_text_width(const char *s, int scale)
{
    if (s == NULL) {
        return 0;
    }
    return (int)strlen(s) * UI_FONT_W * scale;
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

    for (const char *p = s; *p != '\0'; p++, x += char_step) {
        if (x + char_step <= 0 || x >= fb_w || y + glyph_h <= 0 || y >= fb_h) {
            continue; /* whole glyph outside the canvas */
        }

        int idx = (*p >= 0x20 && *p <= 0x7E) ? (*p - 0x20) : ('?' - 0x20);
        const uint8_t *glyph = app_overlay_font8x8[idx];

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
    int len = (int)strlen(s);
    if (len <= max_chars) {
        snprintf(out, out_size, "%s", s);
        return len;
    }
    if (max_chars < 3) {
        out[0] = '\0';
        return 0;
    }
    snprintf(out, out_size, "%.*s..", max_chars - 2, s);
    return max_chars;
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

void ui_button(uint16_t *fb, int fb_w, int fb_h,
               const ui_button_t *b, bool pressed)
{
    uint16_t bg  = pressed ? UI_COLOR_BG_PRESS : UI_COLOR_BG_BTN;
    uint16_t txt = pressed ? UI_COLOR_ACCENT  : UI_COLOR_FG;

    ui_fill_rect(fb, fb_w, fb_h, b->x, b->y, b->x + b->w, b->y + b->h, bg);
    ui_frame(fb, fb_w, fb_h, b->x, b->y, b->x + b->w, b->y + b->h, 2, UI_COLOR_BORDER);

    char fitted[24];
    ui_text_fit(fitted, sizeof(fitted), b->label, b->w - 4, UI_SCALE_BTN);
    int text_w = ui_text_width(fitted, UI_SCALE_BTN);
    int tx = b->x + (b->w - text_w) / 2;
    int ty = b->y + (b->h - UI_FONT_H * UI_SCALE_BTN) / 2;
    ui_text(fb, fb_w, fb_h, tx, ty, fitted, UI_SCALE_BTN, txt, bg);
}

bool ui_button_hit(const ui_button_t *b, int x, int y)
{
    return (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h);
}

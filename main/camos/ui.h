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

#ifndef CAMOS_UI_H
#define CAMOS_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_FONT_W   8
#define UI_FONT_H   8

#define UI_SCALE_TEXT   2   /* body text: 16 px tall */
#define UI_SCALE_TITLE  3   /* titles:     24 px tall */
#define UI_SCALE_BTN    2

/* RGB565 palette */
#define UI_COLOR_BG         0x0000  /* black            */
#define UI_COLOR_FG         0xFFFF  /* white            */
#define UI_COLOR_BORDER     0x7BEF  /* grey             */
#define UI_COLOR_ACCENT     0x07FF  /* cyan             */
#define UI_COLOR_URL        0x867F  /* dim yellow       */
#define UI_COLOR_BG_BTN     0x2104  /* dark blue-grey   */
#define UI_COLOR_BG_PRESS   0x4208  /* pressed button   */
#define UI_COLOR_BAR        0x0841  /* status bar navy  */
#define UI_COLOR_OK         0x07E0  /* green            */
#define UI_COLOR_ERR        0xF800  /* red              */

typedef struct {
    int         x, y, w, h;
    const char *label;
    int         id;
} ui_button_t;

void ui_fill_rect(uint16_t *fb, int fb_w, int fb_h,
                  int x0, int y0, int x1, int y1, uint16_t color);

void ui_frame(uint16_t *fb, int fb_w, int fb_h,
              int x0, int y0, int x1, int y1, int thick, uint16_t color);

int  ui_text_width(const char *s, int scale);

/* Clipped text; returns x coordinate after the last glyph */
int  ui_text(uint16_t *fb, int fb_w, int fb_h,
             int x, int y, const char *s, int scale,
             uint16_t fg, uint16_t bg);

/* Truncate s to fit max_px at the given scale into out (NUL-terminated,
 * ".." appended when truncated); returns chars consumed from s */
int  ui_text_fit(char *out, size_t out_size, const char *s, int max_px, int scale);

/* Text on a filled box */
void ui_box_text(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, const char *s, int scale,
                 uint16_t fg, uint16_t bg, uint16_t box, int pad);

void ui_button(uint16_t *fb, int fb_w, int fb_h,
               const ui_button_t *b, bool pressed);

bool ui_button_hit(const ui_button_t *b, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* CAMOS_UI_H */

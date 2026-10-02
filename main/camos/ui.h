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
#define UI_SCALE_URL    3   /* query row:  24 px tall */
#define UI_SCALE_BTN    2

/* --- RGB565 palette (theme v2) ------------------------------------------ */
#define UI_COLOR_BG         0x0884  /* deep navy      #0B1220 */
#define UI_COLOR_PANEL      0x10E7  /* card           #131F38 */
#define UI_COLOR_KEY        0x1949  /* key fill       #1B2B4F */
#define UI_COLOR_BORDER     0x21AB  /* card border    #24365E */
#define UI_COLOR_FG         0xEF7F  /* main text      #E8EEFC */
#define UI_COLOR_DIM        0x9D9B  /* dim text       #9DB1D9 */
#define UI_COLOR_ACCENT     0x569F  /* cyan           #57D0FF */
#define UI_COLOR_ACCENT2    0x3C7F  /* blue           #3F8CFF */
#define UI_COLOR_OK         0x2E6E  /* green          #2ECC71 */
#define UI_COLOR_ERR        0xFA8A  /* red            #FF5252 */
#define UI_COLOR_WARN       0xFDAA  /* amber          #FFB454 */

/* Legacy names kept for older call sites, re-valued to the v2 theme */
#define UI_COLOR_URL        UI_COLOR_DIM
#define UI_COLOR_BAR        0x10C6  /* status bar     #101B33 */
#define UI_COLOR_BG_BTN     UI_COLOR_KEY
#define UI_COLOR_BG_PRESS   0x2BCD  /* pressed key    #2A3D6B */

/* Corner radius presets */
#define UI_R_CARD   10
#define UI_R_KEY    8
#define UI_R_PILL   12

typedef struct {
    int         x, y, w, h;
    const char *label;
    int         id;
    int         scale;      /* label font scale; 0 = UI_SCALE_BTN */
} ui_button_t;

void ui_fill_rect(uint16_t *fb, int fb_w, int fb_h,
                  int x0, int y0, int x1, int y1, uint16_t color);

void ui_frame(uint16_t *fb, int fb_w, int fb_h,
              int x0, int y0, int x1, int y1, int thick, uint16_t color);

/* Rounded filled rectangle (corner radius r; r<=0 degrades to a rect) */
void ui_rrect(uint16_t *fb, int fb_w, int fb_h,
              int x0, int y0, int x1, int y1, int r, uint16_t color);

/* Rounded outline (1 px thick) */
void ui_rrect_frame(uint16_t *fb, int fb_w, int fb_h,
                    int x0, int y0, int x1, int y1, int r, uint16_t color);

/* Filled circle */
void ui_circle(uint16_t *fb, int fb_w, int fb_h,
               int cx, int cy, int r, uint16_t color);

/* Card: panel fill + border. pressed=true brightens the fill */
void ui_card(uint16_t *fb, int fb_w, int fb_h,
             int x0, int y0, int x1, int y1, bool pressed);

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

/* Transparent 16x16 1-bit icon (see camos/ui_icons.h), pixel-doubled at scale 2 */
struct ui_icon;
void ui_icon(const struct ui_icon *ic, uint16_t *fb, int fb_w, int fb_h,
             int x, int y, int scale, uint16_t fg);

/* Rounded progress bar: track on panel, fill frac 0..1 in color */
void ui_progress(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, int w, int h, float frac, uint16_t color);

/* Three-dot spinner, phase rotates the dots; returns nothing */
void ui_spinner(uint16_t *fb, int fb_w, int fb_h,
                int cx, int cy, int r, int phase, uint16_t color);

void ui_button(uint16_t *fb, int fb_w, int fb_h,
               const ui_button_t *b, bool pressed);

bool ui_button_hit(const ui_button_t *b, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* CAMOS_UI_H */

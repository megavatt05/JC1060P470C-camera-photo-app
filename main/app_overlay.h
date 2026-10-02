/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#ifndef APP_OVERLAY_H
#define APP_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ASCII 0x20..0x7E glyph table, one byte per row, LSB = leftmost pixel.
 * Shared with the CamBrowser UI (camos/ui.c). */
extern const uint8_t app_overlay_font8x8[95][8];

/* Cyrillic glyph table (same 8x8/LSB format), generated from DejaVu Sans
 * Mono. Index 0..31 = U+0410..U+042F (А..Я), 32..63 = U+0430..U+044F
 * (а..я), 64 = U+0401 (Ё), 65 = U+0451 (ё). Shared with camos/ui.c. */
extern const uint8_t app_overlay_font8x8_cyr[66][8];

/**
 * @brief Draw the FPS readout into an RGB565 LCD frame buffer.
 *
 * Call after the PPA blit and before esp_lcd_panel_draw_bitmap(),
 * on the buffer that is about to be displayed. The value is drawn in
 * the top-left corner inside a black box, ~24 px tall glyphs.
 *
 * @param fb    Frame buffer (EXAMPLE_LCD_H_RES x EXAMPLE_LCD_V_RES, RGB565)
 * @param fb_w  Frame buffer width in pixels
 * @param fb_h  Frame buffer height in pixels
 * @param fps   Frames per second to display (clamped to 0.0..999.9)
 */
void app_overlay_draw_fps(uint16_t *fb, int fb_w, int fb_h, float fps);

#ifdef __cplusplus
}
#endif

#endif /* APP_OVERLAY_H */

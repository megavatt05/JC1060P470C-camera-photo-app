/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_LCD_H
#define APP_LCD_H

/* Self-contained header: touch.c / browser.c include it before any esp_lcd
 * header, so the panel handle type must be pulled in here. */
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_JC8012P4A1

#define EXAMPLE_LCD_H_RES                   (800)
#define EXAMPLE_LCD_V_RES                   (1280)

#elif CONFIG_BOARD_TYPE_JC1060P470

/* JD9165 panel: 7" IPS 1024x600 (docs/CAMERA_DISPLAY.md).
 * MUST match video_timing.v_size in app_lcd.c: the DPI driver allocates
 * each frame buffer as timing.h_size x timing.v_size x bpp. A larger value
 * here makes the PPA write past the end of every frame buffer (with 608 it
 * overran the buffer by 17,408 bytes every frame -> heap corruption,
 * esp_vfs_ioctl crash and a flickering band at the top of the screen). */
#define EXAMPLE_LCD_H_RES                   (1024)
#define EXAMPLE_LCD_V_RES                   (600)

#elif CONFIG_BOARD_TYPE_JC4880P443

#define EXAMPLE_LCD_H_RES                   (480)
#define EXAMPLE_LCD_V_RES                   (800)

#endif

/* Number of DPI panel frame buffers (num_fbs). CamBrowser alternates
 * between fb 0 and fb 1, so 2 is exactly what the browser needs. */
#define EXAMPLE_LCD_BUF_NUM                 (2)

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
#define EXAMPLE_LCD_BIT_PER_PIXEL           (16)
#define EXAMPLE_MIPI_DPI_PX_FORMAT          (LCD_COLOR_PIXEL_FORMAT_RGB565)
#elif CONFIG_LCD_PIXEL_FORMAT_RGB888
#define EXAMPLE_LCD_BIT_PER_PIXEL           (24)
#define EXAMPLE_MIPI_DPI_PX_FORMAT          (LCD_COLOR_PIXEL_FORMAT_RGB888)
#endif

/**
 * @brief Initialize the LCD panel.
 *
 * This function initializes the LCD panel with the provided panel handle. It powers on the LCD,
 * installs the LCD driver, configures the bus, and sets up the panel.
 *
 * @param panel_handle Pointer to the LCD panel handle
 * @return
 *    - ESP_OK: Success
 *    - ESP_FAIL: Failure
 */
esp_err_t app_lcd_init(esp_lcd_panel_handle_t *panel_handle);

/**
 * @brief Get one of the DPI panel's own frame buffers (full-screen size).
 *
 * @param index Buffer index, 0 .. EXAMPLE_LCD_BUF_NUM-1
 * @param out_fb Receives the frame buffer pointer (RGB565, H_RES x V_RES)
 */
void app_lcd_get_fb(int index, void **out_fb);

/**
 * @brief Send the frame buffer to the panel (full-screen draw).
 *
 * @param index Buffer index, 0 .. EXAMPLE_LCD_BUF_NUM-1
 */
void app_lcd_flush(int index);

/**
 * @brief Panel handle for consumers that drive the panel themselves
 *        (the video player's render backend). NULL before app_lcd_init().
 */
esp_lcd_panel_handle_t app_lcd_get_panel(void);

/**
 * @brief DBI panel IO handle (command channel); NULL before app_lcd_init().
 */
esp_lcd_panel_io_handle_t app_lcd_get_io(void);

/**
 * @brief Reset the DPI panel event callbacks to "none".
 *
 * esp_video_render's LCD backend registers .on_color_trans_done on our panel
 * (with the backend object as user_ctx) and never unregisters it on destroy
 * (verified in esp_video_render 1.0.0 and 1.1.0). Once the backend is freed,
 * the next app_lcd_flush() invokes the stale callback with a freed context
 * (xSemaphoreGive on poisoned heap -> LoadProhibited crash right after a
 * video stops). Call this after the video render is destroyed; a later
 * video start re-registers its own callbacks.
 */
void app_lcd_dpi_clear_callbacks(void);

#endif

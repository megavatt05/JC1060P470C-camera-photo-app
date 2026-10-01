/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser API.
 */

#ifndef BROWSER_H
#define BROWSER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the CamBrowser (Ethernet, touch and the UI task).
 *
 * Standalone application mode: the camera subsystem is never initialized
 * and the browser owns the display from the very first frame. Call once
 * from app_main after app_lcd_init().
 */
void browser_start(void);

#ifdef __cplusplus
}
#endif

#endif /* BROWSER_H */

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
 * The browser task stops the camera stream ~1s after boot and takes over
 * the display. Call once from app_main after app_video_stream_task_start().
 *
 * @param video_fd Camera video device fd (used to stop the preview stream)
 */
void browser_start(int video_fd);

#ifdef __cplusplus
}
#endif

#endif /* BROWSER_H */

/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser playback diagnostics: on-screen statistics that answer
 * "why does it stutter - the link, the server, or the CPU?".
 *
 * Collected (1 Hz sampler task, see pstats.c):
 *   - net RX rate      - real bytes/sec arriving from the NIC (lwIP netif
 *                        input hook, runtime wrap, no sdkconfig needed);
 *   - CPU load/core    - exact, from FreeRTOS run-time stats (needs
 *                        CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; otherwise
 *                        the OSD shows "n/a" for this row);
 *   - render FPS       - counted in the video render's write_fb (wrapped
 *                        LCD backend in media_player.c);
 *   - buffering        - BUFFERING/BUFFERED events from the player;
 *   - media clock rate - play-time advance per wall second (0.4x = the
 *                        decoder is starved, whatever the cause).
 *
 * The OSD itself is drawn INTO the video frame buffer by
 * pstats_osd_draw() so it stays visible while the render owns the panel.
 *
 * How to read it:
 *   net low, CPU low            -> link/server is the bottleneck;
 *   CPU1 ~100%, КАДР < target   -> SW H264 decode cannot keep up;
 *   net fine, CPU fine, СТОПЫ   -> server jitter (rare with archive.org).
 */

#ifndef PSTATS_H
#define PSTATS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSTATS_CPU_NA  255u     /* CPU load: runtime stats not compiled in */

typedef struct {
    uint32_t seq;               /* bumped on every sampler update        */
    uint32_t net_kbps;          /* RX kilobytes per second (rounded)     */
    uint32_t cpu0;              /* core 0 load %, PSTATS_CPU_NA = n/a    */
    uint32_t cpu1;              /* core 1 load %, PSTATS_CPU_NA = n/a    */
    uint32_t fps_now;           /* rendered frames per second            */
    uint32_t fps_target;        /* stream fps from the render pipeline   */
    uint32_t stalls;            /* completed re-buffering episodes       */
    bool     buffering_now;     /* currently in a re-buffering pause     */
    uint32_t pos_s;             /* playback clock, seconds               */
    uint32_t dur_s;             /* stream duration, seconds (0 = n/a)    */
    uint32_t rate_x100;         /* media clock speed x100 (40 = 0.4x)    */
} pstats_snapshot_t;

/** @brief Start the 1 Hz sampler + attach the NIC RX counter. Idempotent. */
void pstats_init(void);

/**
 * @brief Feed player lifecycle events (call from the player event callback
 *        for every event; unknown events are ignored).
 * @param event_type esp_player_event_t value as plain int.
 */
void pstats_player_event(int event_type);

/** @brief Zero per-stream counters (call before starting a new stream). */
void pstats_stream_reset(void);

/** @brief Count one rendered video frame; fps = pipeline target fps. */
void pstats_note_video_frame(uint32_t fps);

/** @brief Show/hide the OSD (set from media_player on video start/stop). */
void pstats_osd_set(bool on);
bool pstats_osd_get(void);

/** @brief Copy the latest snapshot; false until the first sample is in. */
bool pstats_get(pstats_snapshot_t *out);

/** @brief Draw the OSD block into an RGB565 frame buffer (video frames).
 *         Cheap enough to call from the render task on every frame. */
void pstats_osd_draw(uint16_t *fb, int fb_w, int fb_h);

#ifdef __cplusplus
}
#endif

#endif /* PSTATS_H */

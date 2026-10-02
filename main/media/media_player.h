/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser media player: internet radio + video, one wrapper API.
 *
 * Built on Espressif's esp_player (esp-gmf) which runs the full
 * demux -> decode -> render pipeline:
 *   radio: HTTP(S) audio stream (raw MP3/AAC ES, or container) decoded and
 *          rendered through esp_audio_render into the board ES8311;
 *   video: MP4 (H.264 + AAC) over HTTP(S) decoded and rendered through
 *          esp_video_render onto the DPI panel frame buffers (LCD backend),
 *          with the PPA doing scale/color conversion on ESP32-P4.
 *
 * Screen ownership: while a video plays, the video render backend owns the
 * DPI frame buffers - the browser UI task must not draw. media_video_start()
 * raises a flag the browser polls; media_stop() clears it after teardown.
 */

#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_STATE_IDLE = 0,       /* nothing loaded / stopped              */
    MEDIA_STATE_CONNECTING,     /* source opened, waiting for data       */
    MEDIA_STATE_PLAYING,
    MEDIA_STATE_PAUSED,
    MEDIA_STATE_FINISHED,       /* stream ended (radio drop or video EOF) */
    MEDIA_STATE_ERROR,          /* unrecoverable playback error          */
} media_state_t;

/**
 * @brief One-time init of the media stack (memory adapter, demuxers,
 *        decoders, board audio). Called lazily by the start functions.
 */
esp_err_t media_player_init(void);

/** @brief Start an internet-radio stream (audio only). Stops any playback. */
esp_err_t media_radio_start(const char *url);

/** @brief Start a video (MP4 H.264+AAC) over HTTP(S). Stops any playback. */
esp_err_t media_video_start(const char *url);

/** @brief Stop playback and tear the pipeline down. Safe when idle. */
void media_stop(void);

/** @brief Pause; only meaningful while MEDIA_STATE_PLAYING. */
void media_pause(void);

/** @brief Resume from pause. */
void media_resume(void);

media_state_t media_get_state(void);

/**
 * @brief Current playback position/duration (ms). Used by the stats sampler.
 * @return false when no player is running or the sync clock is not ready
 *         yet; *dur_ms is 0 when the source has no duration metadata.
 */
bool media_query_play(uint64_t *pos_ms, uint64_t *dur_ms);

/** @brief Human-readable state for the UI ("СТОП", "ИГРАЕТ", ...). */
const char *media_state_str(void);

/** @brief true while the video render owns the panel frame buffers. */
bool media_video_active(void);

/** @brief URL of the currently loaded source ("" when idle). */
const char *media_get_url(void);

void media_set_volume(int vol);
int  media_get_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_PLAYER_H */

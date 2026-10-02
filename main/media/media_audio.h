/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Board audio device for CamBrowser media (radio / video):
 * I2S TX -> ES8311 DAC -> speaker amplifier.
 *
 * JC1060P470C wiring (verified against working ESPHome configs for this
 * exact board): ES8311 sits on the touch I2C bus (SDA 7 / SCL 8), I2S on
 * GPIO 9 (DOUT) / 10 (LRCK) / 12 (BCLK) / 13 (MCLK), the NS4150-class
 * amplifier enable is GPIO11 (active high, driven by the ES8311 driver
 * through its PA hook).
 *
 * The codec device stays configured for a fixed 48 kHz / 16-bit / stereo
 * output; the esp_audio_render pool (channel/bit/rate converters) sits in
 * front of it, so any decoded stream format is resampled transparently.
 */

#ifndef MEDIA_AUDIO_H
#define MEDIA_AUDIO_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One-time setup: I2S TX channel, ES8311 codec device, PA wiring.
 *
 * Creates the device but does not open the audio path. Reuses the touch
 * controller I2C bus when it is up (the ES8311 shares it), otherwise
 * creates its own master bus on the same pins.
 */
esp_err_t media_audio_init(void);

/** @brief Tear everything down (only needed at shutdown; unused in practice). */
void media_audio_deinit(void);

/**
 * @brief Open the playback path (power the codec + PA at 48k/16/stereo).
 *        No-op while already open.
 */
esp_err_t media_audio_open(void);

/** @brief Close the playback path (mutes codec, powers the PA down). */
void media_audio_close(void);

/** @brief PCM writer handed to esp_audio_render (blocking esp_codec_dev_write). */
int media_audio_write(const uint8_t *pcm, int len);

/** @brief Volume 0..100 (applied immediately, persists across open/close). */
void media_audio_set_volume(int vol);
int  media_audio_get_volume(void);

/** @brief false when audio hardware is not available (build or probe failed). */
bool media_audio_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_AUDIO_H */

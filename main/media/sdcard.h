/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser SD card (media storage): lazy mount of the microSD slot.
 *
 * Board wiring (JC1060P470C / Guition JC-ESP32P4-M3 module, confirmed
 * against independent board configs): SDMMC slot 0, 4-bit bus,
 * CLK 43 / CMD 44 / D0..D3 39..42, card IO power from the ESP32-P4
 * on-chip LDO channel 4 (that is what the slot is wired to on this board
 * family). No card-detect, no write-protect line.
 */

#ifndef MEDIA_SDCARD_H
#define MEDIA_SDCARD_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Mount the card at the configured point. Idempotent: returns
 *         ESP_OK immediately when already mounted (cached result is not
 *         re-checked - call sdcard_reprobe() after card hot-plug). */
esp_err_t sdcard_mount(void);

/** @brief Allow the next sdcard_mount() to retry after a failed attempt.
 *         No-op while the card is mounted: the live VFS/host/LDO are kept
 *         (re-acquiring the LDO channel would fail and kill playback) -
 *         a refresh just re-reads the directory. */
void sdcard_reprobe(void);

/** @brief true when a card is mounted right now. */
bool sdcard_mounted(void);

/** @brief Mount point path ("" when CONFIG_EB_SD_ENABLE is off). */
const char *sdcard_mp(void);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_SDCARD_H */

/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser touch input API: auto-probe + polling.
 * See camos/touch.c for the controller list and register details.
 */

#ifndef CAMOS_TOUCH_H
#define CAMOS_TOUCH_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
    int id;
} touch_point_t;

/**
 * @brief Probe the I2C bus for a known touch controller.
 *
 * Candidates: GT911 (0x5D/0x14), FT5x06 (0x38), CST816 (0x15).
 *
 * @return
 *    - ESP_OK: controller found and ready
 *    - ESP_ERR_NOT_FOUND: nobody answered (browser runs without input)
 *    - ESP_FAIL: I2C bus could not be initialized
 */
esp_err_t touch_init(void);

/**
 * @brief Poll current touch points (coordinates already normalized to the
 *        LCD resolution, swap/mirror Kconfig flags applied).
 *
 * @param out     Array to fill
 * @param max_out Array capacity (use TOUCH_MAX_POINTS)
 * @return number of points, 0 if untouched, -1 on I2C error
 */
int touch_poll(touch_point_t *out, int max_out);

#define TOUCH_MAX_POINTS 5

/** @brief Chip name for the status bar: "GT911"/"FT5x06"/"CST816"/"none" */
const char *touch_chip_name(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMOS_TOUCH_H */

/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_cam_sensor_types.h"

#define OV02C10_SCCB_ADDR   0x36

/* OV02C10 SCCB/I2C addressing notes (research of public sources, see
 * docs/OV02C10_CAPABILITIES.md):
 * - 0x36 (7-bit) is the primary address, used by all ESP32-P4 boards
 *   (GUITION JC1060P470C and others).
 * - OmniVision sensors commonly provide a second SCCB address selected by
 *   the SID strap; for this sensor family the alternate is 0x10.
 * - Laptop implementations (ACPI HID OVTI02C1: Dell XPS, Samsung Galaxy
 *   Book) define the address in the BIOS DSDT per model.
 * - This driver is registered at OV02C10_SCCB_ADDR; esp_video additionally
 *   scans the I2C bus and re-runs detection at every responding address,
 *   so a module strapped differently is picked up automatically. */

/**
 * @brief Power on camera sensor device and detect the device connected to the designated sccb bus.
 *
 * @param[in] config Configuration related to device power-on and detection.
 * @return
 *      - Camera device handle on success, otherwise, failed.
 */
esp_cam_sensor_device_t *ov02c10_detect(esp_cam_sensor_config_t *config);

#ifdef __cplusplus
}
#endif

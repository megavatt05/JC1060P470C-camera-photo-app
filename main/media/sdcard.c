/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser SD card mount (media storage), see sdcard.h.
 *
 * Mount is lazy (first call from the media file screen) so a missing card
 * never slows down boot. The bus runs at 20 MHz by default - safe for the
 * on-board wiring; raise EB_SD_MAX_FREQ_KHZ after validating the card.
 */

#include <string.h>
#include <sys/unistd.h>
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_EB_MEDIA_ENABLE && CONFIG_EB_SD_ENABLE

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "sdcard";

static struct {
    bool           mounted;         /* mount attempt succeeded            */
    bool           probed;          /* an attempt was made                */
    sdmmc_card_t  *card;
    sd_pwr_ctrl_handle_t ldo;       /* on-chip LDO powering the card IO   */
} s_sd;

esp_err_t sdcard_mount(void)
{
    if (s_sd.probed) {
        return s_sd.mounted ? ESP_OK : ESP_FAIL;
    }
    s_sd.probed = true;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = CONFIG_EB_SD_MAX_FREQ_KHZ;

    /* the card IO rail is fed by the P4 on-chip LDO channel
     * CONFIG_EB_SD_LDO_CHAN on this board */
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = CONFIG_EB_SD_LDO_CHAN,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_sd.ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "on-chip LDO init failed: %s", esp_err_to_name(err));
        return err;
    }
    host.pwr_ctrl_handle = s_sd.ldo;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = CONFIG_EB_SD_CLK_GPIO;
    slot.cmd = CONFIG_EB_SD_CMD_GPIO;
    slot.d0  = CONFIG_EB_SD_D0_GPIO;
    slot.d1  = CONFIG_EB_SD_D1_GPIO;
    slot.d2  = CONFIG_EB_SD_D2_GPIO;
    slot.d3  = CONFIG_EB_SD_D3_GPIO;
    slot.width = 4;
    /* the board has no external pullups on the SD lines; internal ones are
     * enough at 20 MHz for card init (same approach as the IDF example) */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 64 * 1024,
    };

    ESP_LOGI(TAG, "mounting SD (slot0 4-bit, clk=%d cmd=%d d0..d3=%d..%d, "
             "LDO ch%d)...", CONFIG_EB_SD_CLK_GPIO, CONFIG_EB_SD_CMD_GPIO,
             CONFIG_EB_SD_D0_GPIO, CONFIG_EB_SD_D3_GPIO, CONFIG_EB_SD_LDO_CHAN);
    err = esp_vfs_fat_sdmmc_mount(CONFIG_EB_SD_MOUNT, &host, &slot,
                                  &mcfg, &s_sd.card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (FAT32 card in the slot?)",
                 esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(s_sd.ldo);
        s_sd.ldo = NULL;
        return err;
    }

    s_sd.mounted = true;
    sdmmc_card_print_info(stdout, s_sd.card);
    ESP_LOGI(TAG, "SD mounted at %s", CONFIG_EB_SD_MOUNT);
    return ESP_OK;
}

void sdcard_reprobe(void)
{
    s_sd.probed = false;
    s_sd.mounted = false;
}

bool sdcard_mounted(void)
{
    return s_sd.mounted;
}

const char *sdcard_mp(void)
{
    return CONFIG_EB_SD_MOUNT;
}

#else /* !media || !sd */

#include "stdbool.h"

esp_err_t sdcard_mount(void)      { return ESP_ERR_NOT_SUPPORTED; }
void     sdcard_reprobe(void)     { }
bool     sdcard_mounted(void)     { return false; }
const char *sdcard_mp(void)       { return ""; }

#endif

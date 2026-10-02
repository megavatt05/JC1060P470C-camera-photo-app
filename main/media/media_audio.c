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

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "sdkconfig.h"
#include "camos/touch.h"
#include "media/media_audio.h"

static const char *TAG = "media_audio";

#define AU_OUT_RATE     48000
#define AU_OUT_BITS     16
#define AU_OUT_CH       2

static struct {
    bool            inited;
    i2s_chan_handle_t tx;
    i2c_master_bus_handle_t own_bus;    /* created only when touch has no bus */
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_data_if_t *data_if;
    const audio_codec_if_t *codec_if;
    esp_codec_dev_handle_t dev;
    bool open;
    bool wr_err;                        /* logged a write failure recently */
    int volume;
} s_au = { .volume = 70 };

static esp_err_t au_i2s_channel_create(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_EB_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;     /* silence on underrun instead of a glitch loop */
    /* ~40 ms of buffering at 48k/16/stereo: smooths decoder jitter without
     * eating internal RAM (8 descs * 460 frames * 4 B = ~14.7 KB). */
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 460;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_au.tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AU_OUT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)CONFIG_EB_AUDIO_I2S_MCLK,
            .bclk = (gpio_num_t)CONFIG_EB_AUDIO_I2S_BCLK,
            .ws   = (gpio_num_t)CONFIG_EB_AUDIO_I2S_LRCK,
            .dout = (gpio_num_t)CONFIG_EB_AUDIO_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    if (CONFIG_EB_AUDIO_I2S_MCLK < 0) {
        std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    }

    err = i2s_channel_init_std_mode(s_au.tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        i2s_del_channel(s_au.tx);
        s_au.tx = NULL;
        return err;
    }
    return ESP_OK;
}

static esp_err_t au_codec_create(void)
{
    /* I2C control: prefer the bus touch_init() already created - the ES8311
     * shares it with the GT911, and a port can hold only one master bus. */
    void *bus = touch_get_i2c_bus();
    if (bus == NULL) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = 1,
            .sda_io_num = (gpio_num_t)CONFIG_EB_TOUCH_I2C_SDA,
            .scl_io_num = (gpio_num_t)CONFIG_EB_TOUCH_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&bus_cfg, &s_au.own_bus) == ESP_OK) {
            bus = s_au.own_bus;
        } else {
            ESP_LOGW(TAG, "no I2C bus for the codec (touch owns none, own init failed)");
            return ESP_FAIL;
        }
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = (uint8_t)CONFIG_EB_AUDIO_CODEC_ADDR,
        .bus_handle = bus,
        .clock_speed_hz = 100000,
    };
    s_au.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_au.ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = (uint8_t)CONFIG_EB_AUDIO_I2S_PORT,
        .tx_handle = (void *)s_au.tx,
        .rx_handle = NULL,
    };
    s_au.data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_au.data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = s_au.ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = (int16_t)CONFIG_EB_AUDIO_PA_GPIO,
#if CONFIG_EB_AUDIO_PA_ACTIVE_HIGH
        .pa_reverted = false,
#else
        .pa_reverted = true,
#endif
        .master_mode = false,
        .use_mclk = (CONFIG_EB_AUDIO_I2S_MCLK >= 0),
    };
    s_au.codec_if = es8311_codec_new(&es_cfg);
    if (s_au.codec_if == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new failed (codec not on bus?)");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_au.codec_if,
        .data_if = s_au.data_if,
    };
    s_au.dev = esp_codec_dev_new(&dev_cfg);
    if (s_au.dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t media_audio_init(void)
{
    if (s_au.inited) {
        return ESP_OK;
    }

    esp_err_t err = au_i2s_channel_create();
    if (err != ESP_OK) {
        return err;
    }
    err = au_codec_create();
    if (err != ESP_OK) {
        return err;
    }

    s_au.inited = true;
    ESP_LOGI(TAG, "audio ready: ES8311 @0x%02X, I2S%d dout=%d ws=%d bclk=%d mclk=%d pa=%d",
             (unsigned)CONFIG_EB_AUDIO_CODEC_ADDR, CONFIG_EB_AUDIO_I2S_PORT,
             CONFIG_EB_AUDIO_I2S_DOUT, CONFIG_EB_AUDIO_I2S_LRCK,
             CONFIG_EB_AUDIO_I2S_BCLK, CONFIG_EB_AUDIO_I2S_MCLK,
             CONFIG_EB_AUDIO_PA_GPIO);
    return ESP_OK;
}

void media_audio_deinit(void)
{
    if (!s_au.inited) {
        return;
    }
    media_audio_close();
    if (s_au.dev != NULL) {
        esp_codec_dev_delete(s_au.dev);
        s_au.dev = NULL;
    }
    s_au.inited = false;
}

esp_err_t media_audio_open(void)
{
    if (!s_au.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_au.open) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AU_OUT_RATE,
        .channel = AU_OUT_CH,
        .bits_per_sample = AU_OUT_BITS,
    };
    esp_err_t err = esp_codec_dev_open(s_au.dev, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open: %d", err);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_au.dev, s_au.volume);
    s_au.wr_err = false;
    s_au.open = true;
    return ESP_OK;
}

void media_audio_close(void)
{
    if (s_au.inited && s_au.open && s_au.dev != NULL) {
        esp_codec_dev_close(s_au.dev);
        s_au.open = false;
    }
}

int media_audio_write(const uint8_t *pcm, int len)
{
    if (!s_au.open || s_au.dev == NULL) {
        if (!s_au.wr_err) {
            ESP_LOGE(TAG, "write: audio device is not open");
            s_au.wr_err = true;
        }
        return -1;
    }
    int ret = esp_codec_dev_write(s_au.dev, (void *)pcm, len);
    if (ret < 0) {
        /* log the first failure only - a dead stream would otherwise spam */
        if (!s_au.wr_err) {
            ESP_LOGE(TAG, "codec write failed: %d", ret);
            s_au.wr_err = true;
        }
        return -1;
    }
    s_au.wr_err = false;
    return len;
}

void media_audio_set_volume(int vol)
{
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_au.volume = vol;
    if (s_au.open && s_au.dev != NULL) {
        esp_codec_dev_set_out_vol(s_au.dev, s_au.volume);
    }
}

int media_audio_get_volume(void)
{
    return s_au.volume;
}

bool media_audio_ready(void)
{
    return s_au.inited;
}

/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser media player on top of esp_player (esp-gmf).
 * See media_player.h for the architecture notes.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "app_lcd.h"
#include "media_lib_adapter.h"
#include "esp_extractor_defaults.h"
#include "esp_audio_dec_default.h"
#include "esp_video_dec_default.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_alc.h"
#include "esp_audio_render.h"
#include "esp_video_render.h"
#include "esp_video_render_backend.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_gmf_video_ppa.h"
#endif
#include "esp_gmf_video_color_convert.h"
#include "esp_player.h"
#include "esp_player_advance.h"
#include "media/media_audio.h"
#include "media/media_player.h"

static const char *TAG = "media_player";

#define MEDIA_URL_MAX 256

/* esp_audio_render write callback -> codec device.
 * Contract (esp_audio_render_write_cb_t): return 0 on success, non-zero on
 * failure. Returning the byte count here made the render treat every
 * successful write as an error ("OUT port release error, ret:-1" from
 * ESP_GMF_RATE_CVT) and killed the stream on the first PCM chunk. */
static int audio_writer_cb(uint8_t *pcm, uint32_t len, void *ctx)
{
    (void)ctx;
    return media_audio_write(pcm, (int)len) < 0 ? -1 : 0;
}

static struct {
    bool           inited;              /* stack + audio registered  */
    volatile media_state_t state;
    bool           mode_video;          /* last start was a video    */
    volatile bool  video_active;        /* render owns the panel     */
    char           url[MEDIA_URL_MAX];

    esp_player_handle_t player;

    /* audio render (shared by radio and video) */
    esp_gmf_pool_handle_t audio_pool;
    esp_audio_render_handle_t audio_render;
    esp_audio_render_stream_handle_t audio_stream;

    /* video render (video only) */
    esp_gmf_pool_handle_t video_pool;
    esp_video_render_handle_t video_render;
} s_mp;

static esp_player_err_t player_event_cb(esp_player_event_msg_t *msg, void *ctx)
{
    (void)ctx;
    switch (msg->event_type) {
    case ESP_PLAYER_EVENT_PLAYED:
        s_mp.state = MEDIA_STATE_PLAYING;
        ESP_LOGI(TAG, "played");
        break;
    case ESP_PLAYER_EVENT_PAUSED:
        s_mp.state = MEDIA_STATE_PAUSED;
        ESP_LOGI(TAG, "paused");
        break;
    case ESP_PLAYER_EVENT_BUFFERING:
        ESP_LOGI(TAG, "buffering...");
        break;
    case ESP_PLAYER_EVENT_BUFFERED:
        ESP_LOGI(TAG, "buffered");
        break;
    case ESP_PLAYER_EVENT_AUDIO_INFO_PARSED:
        ESP_LOGI(TAG, "audio track parsed");
        break;
    case ESP_PLAYER_EVENT_VIDEO_INFO_PARSED:
        ESP_LOGI(TAG, "video track parsed");
        break;
    case ESP_PLAYER_EVENT_FINISHED:
        s_mp.state = MEDIA_STATE_FINISHED;
        ESP_LOGI(TAG, "finished");
        break;
    case ESP_PLAYER_EVENT_STOPPED:
        /* teardown is owned by media_stop(); report idle only if the
         * player stopped on its own (error recovery paths) */
        if (s_mp.state == MEDIA_STATE_PLAYING || s_mp.state == MEDIA_STATE_CONNECTING) {
            s_mp.state = MEDIA_STATE_FINISHED;
        }
        ESP_LOGI(TAG, "stopped");
        break;
    case ESP_PLAYER_EVENT_ERROR:
        s_mp.state = MEDIA_STATE_ERROR;
        if (msg->data != NULL) {
            ESP_LOGE(TAG, "playback error, source=%d",
                     (int)*(esp_player_error_source_t *)msg->data);
        } else {
            ESP_LOGE(TAG, "playback error");
        }
        break;
    default:
        break;
    }
    return ESP_PLAYER_ERR_OK;
}

static void destroy_audio_render(void)
{
    if (s_mp.audio_render != NULL) {
        esp_audio_render_destroy(s_mp.audio_render);
        s_mp.audio_render = NULL;
        s_mp.audio_stream = NULL;
    }
    if (s_mp.audio_pool != NULL) {
        esp_gmf_pool_deinit(s_mp.audio_pool);
        s_mp.audio_pool = NULL;
    }
}

static esp_err_t create_audio_render(void)
{
    if (s_mp.audio_render != NULL) {
        return ESP_OK;      /* already up */
    }

    esp_err_t err = media_audio_open();
    if (err != ESP_OK) {
        return err;
    }

    /* GMF pool with the converters between any decoded stream format and
     * the fixed 48k/16/stereo codec device. */
    if (esp_gmf_pool_init(&s_mp.audio_pool) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    esp_gmf_element_handle_t el = NULL;

    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&ch_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_mp.audio_pool, el, NULL);
    }
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bit_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_mp.audio_pool, el, NULL);
    }
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_rate_cvt_init(&rate_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_mp.audio_pool, el, NULL);
    }
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_mp.audio_pool, el, NULL);
    }

    esp_audio_render_cfg_t rcfg = {
        .max_stream_num = 1,
        .out_writer = audio_writer_cb,
        .out_ctx = NULL,
        .out_sample_info = {
            .sample_rate = 48000,
            .bits_per_sample = 16,
            .channel = 2,
        },
        .pool = s_mp.audio_pool,
        .process_period = 20,
    };

    if (esp_audio_render_create(&rcfg, &s_mp.audio_render) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_render_create failed");
        destroy_audio_render();
        return ESP_FAIL;
    }
    if (esp_audio_render_stream_get(s_mp.audio_render, ESP_AUDIO_RENDER_FIRST_STREAM,
                                    &s_mp.audio_stream) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_render_stream_get failed");
        destroy_audio_render();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void destroy_video_render(void)
{
    esp_log_level_set("VIDEO_RENDER", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
    if (s_mp.video_render != NULL) {
        esp_video_render_destroy(s_mp.video_render);
        s_mp.video_render = NULL;
    }
    if (s_mp.video_pool != NULL) {
        esp_gmf_pool_deinit(s_mp.video_pool);
        s_mp.video_pool = NULL;
    }
    /* The LCD backend registers .on_color_trans_done on our DPI panel with
     * itself as the context and never unregisters it on close (verified in
     * esp_video_render 1.0.0 and 1.1.0). After esp_video_render_destroy()
     * freed the backend, the next app_lcd_flush() still invoked the stale
     * callback (esp_lcd calls on_color_trans_done synchronously from
     * dpi_panel_draw_bitmap_2d) -> xSemaphoreGive on freed memory ->
     * "Guru Meditation Error: Core 1 panic'ed (Load access fault)" right
     * after a video finished. Clear the registration so the panel is safe
     * for browser UI draws again. */
    app_lcd_dpi_clear_callbacks();
}

static esp_err_t create_video_render(void)
{
    if (s_mp.video_render != NULL) {
        return ESP_OK;
    }

    if (esp_gmf_pool_init(&s_mp.video_pool) != ESP_GMF_ERR_OK) {
        return ESP_FAIL;
    }
    esp_gmf_element_handle_t el = NULL;
#if CONFIG_IDF_TARGET_ESP32P4
    /* PPA: hardware scale/blend/color-convert on the P4 */
    if (esp_gmf_video_ppa_init(NULL, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_mp.video_pool, el, NULL);
    }
#endif
    esp_imgfx_color_convert_cfg_t color_cfg = {
        .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
    };
    if (esp_gmf_video_color_convert_init(&color_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(s_mp.video_pool, el, NULL);
    }

    esp_video_render_cfg_t vcfg = {
        .pool = s_mp.video_pool,
        .fps = 30,
    };
    if (esp_video_render_create(&vcfg, &s_mp.video_render) != ESP_VIDEO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "esp_video_render_create failed");
        destroy_video_render();
        return ESP_FAIL;
    }

    /* LCD backend: reuse the panel's own DPI frame buffers (2 x 1024x600
     * RGB565). From here on the render owns the panel until teardown. */
    esp_video_render_lcd_cfg_t lcd_cfg = {
        .lcd_type = ESP_VIDEO_RENDER_LCD_TYPE_DPI,
        .fb_num = 2,
        .out_format = ESP_VIDEO_RENDER_FORMAT_RGB565,
        .width = EXAMPLE_LCD_H_RES,
        .height = EXAMPLE_LCD_V_RES,
        .lcd_handle = app_lcd_get_panel(),
        .io_handle = app_lcd_get_io(),
    };
    if (lcd_cfg.lcd_handle == NULL) {
        ESP_LOGE(TAG, "panel handle is NULL");
        destroy_video_render();
        return ESP_FAIL;
    }
    esp_video_render_backend_cfg_t backend_cfg = {
        .ops = esp_video_render_get_lcd_backend(),
        .cfg = &lcd_cfg,
        .cfg_size = sizeof(lcd_cfg),
    };
    if (esp_video_render_set_display(s_mp.video_render, &backend_cfg) != ESP_VIDEO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "esp_video_render_set_display failed");
        destroy_video_render();
        return ESP_FAIL;
    }

    /* esp_video_render logs a WARN per late frame ("Write too slow reset rate
     * control") whenever the stream runs slower than realtime - hundreds of
     * lines per minute that each cost CPU on the render core and flood the
     * /log ring. The buffering events + busy card already carry that info:
     * silence the tag for the playback window, restore on teardown. */
    esp_log_level_set("VIDEO_RENDER", ESP_LOG_ERROR);
    return ESP_OK;
}

static void player_teardown(void)
{
    if (s_mp.player != NULL) {
        /* NB: no esp_player_set_event_cb(NULL) here - the player rejects it
         * while PLAYING ("Failed to set event cb. state: 2") and the callback
         * only touches static s_mp fields, so it is safe until deinit. */
        esp_player_stop(s_mp.player);
        esp_player_deinit(s_mp.player);
        s_mp.player = NULL;
    }
    destroy_audio_render();
    destroy_video_render();
    media_audio_close();
    s_mp.video_active = false;
}

static esp_err_t player_start(const char *url, bool video)
{
    /* stop + tear down whatever is running now */
    player_teardown();
    s_mp.state = MEDIA_STATE_CONNECTING;
    s_mp.mode_video = video;
    strlcpy(s_mp.url, url, sizeof(s_mp.url));

    esp_err_t err = create_audio_render();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio render setup failed");
        goto fail;
    }
    if (video) {
        err = create_video_render();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "video render setup failed");
            goto fail;
        }
        /* the render now owns the panel frame buffers */
        s_mp.video_active = true;
    }

    esp_player_config_t pcfg = ESP_PLAYER_CONFIG_DEFAULT();
    pcfg.audio_render_hd = s_mp.audio_stream;
    pcfg.video_render_hd = video ? s_mp.video_render : NULL;
    if (esp_player_init(&pcfg, &s_mp.player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "esp_player_init failed");
        goto fail;
    }
    esp_player_set_event_cb(s_mp.player, player_event_cb, NULL);

    /* The built-in demux pool is 100 KB for video (player_defaults_cfg.h).
     * On a typical archive.org stream it saturates at ~140 ms buffered while
     * the re-buffering gate waits for 300 ms, so the gate gives up and
     * disables itself for the stream ("Demux pool saturated ... disabling
     * the re-buffering gate") - playback then free-runs starved and every
     * late frame logs "Write too slow" from the video render. 512 KB of
     * PSRAM holds seconds of stream data, the gate actually reaches its
     * resume threshold and network jitter gets smoothed out. Allowed in
     * IDLE/STOPPED/FINISHED only - right after init is exactly that. */
    esp_player_buffer_config_t bcfg = {
        .extractor_pool_size = 512 * 1024,
    };
    if (esp_player_set_buffer_config(s_mp.player, &bcfg) != ESP_PLAYER_ERR_OK) {
        ESP_LOGW(TAG, "buffer cfg override failed, keeping built-in defaults");
    }

    /* esp_player pins video_decoder to core 0 by default, so the CPU-bound
     * SW H264 decode (tinyh264) starves IDLE0 -> task watchdog dumps and
     * contention with everything living on core 0. Swap: video decode ->
     * core 1, video render (SW color cvt + PPA + LCD flush) -> core 0.
     * Audio tasks keep the built-in defaults (decoder=1, render=0).
     * Stack/prio mirror player_defaults_cfg.h. */
    esp_player_task_config_t tcfg = {
        .extractor     = { .stack = 5120, .prio = 5, .core = 0, .stack_in_ext = 0 },
        .audio_decoder = { .stack = 5120, .prio = 5, .core = 1, .stack_in_ext = 0 },
        .audio_render  = { .stack = 5120, .prio = 5, .core = 0, .stack_in_ext = 0 },
        .video_decoder = { .stack = 5120, .prio = 5, .core = 1, .stack_in_ext = 0 },
        .video_render  = { .stack = 5120, .prio = 5, .core = 0, .stack_in_ext = 0 },
    };
    esp_player_set_task_config(s_mp.player, &tcfg);

    esp_player_data_src_t src = ESP_PLAYER_DATA_SRC(s_mp.url,
                                       video ? ESP_PLAYER_MASK_AV : ESP_PLAYER_MASK_AUDIO);
    if (esp_player_set_data_src(s_mp.player, &src) != ESP_PLAYER_ERR_OK ||
        esp_player_run(s_mp.player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "player start failed");
        goto fail;
    }

    ESP_LOGI(TAG, "start %s: %s", video ? "video" : "radio", url);
    return ESP_OK;

fail:
    player_teardown();
    s_mp.url[0] = '\0';
    s_mp.state = MEDIA_STATE_ERROR;
    return ESP_FAIL;
}

esp_err_t media_player_init(void)
{
    if (s_mp.inited) {
        return ESP_OK;
    }

    /* media memory adapter + default demuxers/decoders */
    media_lib_add_default_adapter();
    esp_extractor_register_default();
    esp_audio_dec_register_default();
    esp_video_dec_register_default();

    esp_err_t err = media_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board audio init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mp.inited = true;
    return ESP_OK;
}

esp_err_t media_radio_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = media_player_init();
    if (err != ESP_OK) {
        return err;
    }
    return player_start(url, false);
}

esp_err_t media_video_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = media_player_init();
    if (err != ESP_OK) {
        return err;
    }
    return player_start(url, true);
}

void media_stop(void)
{
    player_teardown();
    s_mp.state = MEDIA_STATE_IDLE;
}

void media_pause(void)
{
    if (s_mp.player != NULL && s_mp.state == MEDIA_STATE_PLAYING) {
        esp_player_pause(s_mp.player);
    }
}

void media_resume(void)
{
    if (s_mp.player != NULL && s_mp.state == MEDIA_STATE_PAUSED) {
        esp_player_resume(s_mp.player);
    }
}

media_state_t media_get_state(void)
{
    return s_mp.state;
}

const char *media_state_str(void)
{
    switch (s_mp.state) {
    case MEDIA_STATE_CONNECTING: return "СОЕДИНЕНИЕ...";
    case MEDIA_STATE_PLAYING:    return "ИГРАЕТ";
    case MEDIA_STATE_PAUSED:     return "ПАУЗА";
    case MEDIA_STATE_FINISHED:   return "КОНЕЦ";
    case MEDIA_STATE_ERROR:      return "ОШИБКА";
    default:                     return "СТОП";
    }
}

bool media_video_active(void)
{
    return s_mp.video_active;
}

const char *media_get_url(void)
{
    return s_mp.url;
}

void media_set_volume(int vol)
{
    media_audio_set_volume(vol);
}

int media_get_volume(void)
{
    return media_audio_get_volume();
}

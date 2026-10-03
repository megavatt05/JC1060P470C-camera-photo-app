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
#include "esp_timer.h"
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
#include "esp_task_wdt.h"
#include "freertos/idf_additions.h"
#include "media/media_audio.h"
#include "media/media_player.h"
#include "media/pstats.h"

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

/* Rate-limited BUFFERING/BUFFERED telemetry. At 512x288-class streams the
 * SW decoder throttles the demuxer (full video queue blocks it
 * head-of-line), so the re-buffering gate flaps enter/leave about once
 * per grace period (~4 Hz) while the audio margin stays healthy -
 * hundreds of events per minute, each previously logged (E-24). pstats
 * still feeds СТОПЫ/БУФЕРИЗАЦИЯ on the OSD from every event; the UART log
 * gets one summary line per 10 s instead. */
static uint32_t s_buf_n_enter, s_buf_n_leave;
static int64_t s_buf_last_log_us = -10000000;

static void player_buf_evt_log(bool enter)
{
    int64_t now = esp_timer_get_time();
    if (enter) {
        s_buf_n_enter++;
    } else {
        s_buf_n_leave++;
    }
    if (now - s_buf_last_log_us < 10000000) {
        return;
    }
    s_buf_last_log_us = now;
    if (enter) {
        ESP_LOGI(TAG, "buffering #%lu (buffered #%lu; further gate events rate-limited to 1 line / 10 s)",
                 (unsigned long)s_buf_n_enter, (unsigned long)s_buf_n_leave);
    } else {
        ESP_LOGI(TAG, "buffered #%lu (buffering #%lu; further gate events rate-limited to 1 line / 10 s)",
                 (unsigned long)s_buf_n_leave, (unsigned long)s_buf_n_enter);
    }
}

static void player_buf_evt_reset(void)
{
    s_buf_n_enter = 0;
    s_buf_n_leave = 0;
    s_buf_last_log_us = -10000000;
}

static esp_player_err_t player_event_cb(esp_player_event_msg_t *msg, void *ctx)
{
    (void)ctx;
    pstats_player_event((int)msg->event_type);
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
        player_buf_evt_log(true);
        break;
    case ESP_PLAYER_EVENT_BUFFERED:
        player_buf_evt_log(false);
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

/* --- video render backend wrapper: stats OSD into every frame ---------
 * While a video plays the render owns the DPI frame buffers, so the only
 * way to keep anything on screen is to draw into the very buffer the
 * backend is about to display. We wrap the stock LCD backend: every
 * write_fb first records the frame (render fps counter + stream fps) and
 * paints the pstats OSD block, then delegates. pstats_osd_draw() is a
 * direct blit - ~0.2 ms per frame on the render core. */
static const esp_video_render_backend_ops_t *s_lcd_ops;
static esp_video_render_backend_handle_t s_lcd_backend;
static int s_osd_handle;    /* opaque address used as our backend handle */

static esp_video_render_err_t osd_init(void *cfg, int cfg_size,
                                       esp_video_render_backend_handle_t *backend)
{
    esp_video_render_err_t r = s_lcd_ops->init(cfg, cfg_size, &s_lcd_backend);
    if (r == ESP_VIDEO_RENDER_ERR_OK && backend != NULL) {
        *backend = (esp_video_render_backend_handle_t)&s_osd_handle;
    }
    return r;
}

static bool osd_with_gram(esp_video_render_backend_handle_t backend)
{
    (void)backend;
    return s_lcd_ops->with_gram(s_lcd_backend);
}

static esp_video_render_err_t osd_get_display_info(esp_video_render_backend_handle_t backend,
                                                   esp_video_render_disp_info_t *info)
{
    (void)backend;
    return s_lcd_ops->get_display_info(s_lcd_backend, info);
}

static esp_video_render_err_t osd_get_fb(esp_video_render_backend_handle_t backend,
                                         esp_video_render_fb_t *fb)
{
    (void)backend;
    return s_lcd_ops->get_fb(s_lcd_backend, fb);
}

static esp_video_render_err_t osd_lock_fb(esp_video_render_backend_handle_t backend,
                                          esp_video_render_fb_t *fb, bool lock)
{
    (void)backend;
    return s_lcd_ops->lock_fb(s_lcd_backend, fb, lock);
}

static esp_video_render_err_t osd_write_fb(esp_video_render_backend_handle_t backend,
                                           esp_video_render_fb_t *fb,
                                           const esp_video_render_rect_t *dirty_rect,
                                           const esp_video_render_pos_t *pos)
{
    (void)backend;
    if (fb != NULL && fb->data != NULL) {
        pstats_note_video_frame(fb->info.fps);
        if (fb->info.format == ESP_VIDEO_RENDER_FORMAT_RGB565 &&
            fb->info.width == EXAMPLE_LCD_H_RES &&
            fb->info.height == EXAMPLE_LCD_V_RES) {
            pstats_osd_draw((uint16_t *)fb->data, fb->info.width, fb->info.height);
        }
    }
    return s_lcd_ops->write_fb(s_lcd_backend, fb, dirty_rect, pos);
}

static esp_video_render_err_t osd_deinit(esp_video_render_backend_handle_t backend)
{
    (void)backend;
    esp_video_render_backend_handle_t real = s_lcd_backend;
    s_lcd_backend = NULL;
    return s_lcd_ops->deinit(real);
}

static const esp_video_render_backend_ops_t s_osd_ops = {
    .init              = osd_init,
    .with_gram         = osd_with_gram,
    .get_display_info  = osd_get_display_info,
    .get_fb            = osd_get_fb,
    .lock_fb           = osd_lock_fb,
    .write_fb          = osd_write_fb,
    .deinit            = osd_deinit,
};

static void destroy_video_render(void)
{
    pstats_osd_set(false);
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
    s_lcd_ops = esp_video_render_get_lcd_backend();
    if (s_lcd_ops == NULL) {
        ESP_LOGE(TAG, "no lcd backend ops");
        destroy_video_render();
        return ESP_FAIL;
    }
    esp_video_render_backend_cfg_t backend_cfg = {
        .ops = &s_osd_ops,          /* wraps the lcd ops: stats OSD */
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
     * IDLE/STOPPED/FINISHED only - right after init is exactly that.
     *
     * Network tuning (eab762a follow-up): the built-in gate thresholds are
     * 400/200/300 ms - tuned for fast links. On real archive.org streams
     * the demux pool barely holds one gate cycle ("saturated at 278 ms"),
     * so the gate disables itself and playback free-runs starved. Give it
     * real headroom: 2 MB demux pool + 256 KB HTTP read-ahead + 1.5 s
     * prebuffer (PSRAM is 32 MB; 1.5 s of media is ~400 KB at 2 Mbps) and
     * short network bursts stop stalling the picture.
     *
     * archive.org CDN рвёт длинные TLS (MBEDTLS -0x004C / ENOTCONN).
     * Больше HTTP read-ahead + demux pool → реже starve при реконнекте.
     * (Журнал: E-HTTP-TLS-RECONNECT, 2026-10-03.) */
    esp_player_buffer_config_t bcfg = {
        .extractor_pool_size = 4 * 1024 * 1024,
        .http_read_buf_size  = 512 * 1024,
        .prebuffer_resume_ms = 2000,
        .rebuffer_enter_ms   = 500,
        .rebuffer_resume_ms  = 1500,
        .rebuffer_grace_ms   = 300,
    };
    if (esp_player_set_buffer_config(s_mp.player, &bcfg) != ESP_PLAYER_ERR_OK) {
        ESP_LOGW(TAG, "buffer cfg override failed, keeping built-in defaults");
    }
    pstats_stream_reset();
    player_buf_evt_reset();
    pstats_osd_set(video);

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

bool media_query_play(uint64_t *pos_ms, uint64_t *dur_ms)
{
    if (s_mp.player == NULL) {
        return false;
    }
    uint64_t pos = 0, dur = 0;
    if (esp_player_get_play_time(s_mp.player, &pos) != ESP_PLAYER_ERR_OK) {
        return false;
    }
    if (esp_player_get_duration(s_mp.player, &dur) != ESP_PLAYER_ERR_OK) {
        dur = 0;
    }
    if (pos_ms != NULL) {
        *pos_ms = pos;
    }
    if (dur_ms != NULL) {
        *dur_ms = dur;
    }
    return true;
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

    /* Playback stats: NIC rx counter + 1 Hz sampler (pstats.c) */
    pstats_init();

    /* esp_player's format helper guesses the container from the URL file
     * extension; radio streams have none (.../ep128, icecast mounts), so
     * it logs a scary "Invalid argument. url: ... format: 0x..." and the
     * pipeline just falls back to probing (player_stream.c ignores the
     * error - playback is unaffected). Silence the tag: its remaining
     * messages duplicate failures we already surface ourselves. */
    esp_log_level_set("ESP_PLAYER_HELPER", ESP_LOG_NONE);
    /* CDN reconnect storms (archive.org) — не засоряем UART на каждый Open */
    esp_log_level_set("ESP_GMF_HTTP", ESP_LOG_WARN);

    /* The SW H264 decoder legitimately saturates its core (core 1) for
     * minutes at 360p-class streams; IDLE1 then never runs and the task
     * watchdog dumps registers every ~5 s while playback keeps going.
     * Watch only CPU0's idle task via esp_task_wdt_reconfigure(): decode
     * starvation on core 1 becomes VISIBLE in the on-screen stats (CPU1
     * %, FPS vs target) instead of watchdog dumps. reconfigure() - unlike
     * a bare esp_task_wdt_delete(idle1) - also deregisters the core-1
     * idle hook; with delete alone the hook kept feeding a now-unknown
     * task, and every core-1 idle pass logged "task not found" (E-23).
     * Timeout and panic behaviour mirror the Kconfig unchanged.
     *
     * IDF 6.0.3: CONFIG_ESP_TASK_WDT_PANIC — bool Kconfig. Если опция
     * выключена (# CONFIG_ESP_TASK_WDT_PANIC is not set), макрос в
     * sdkconfig.h НЕ определяется → undeclared. Берём через #ifdef.
     * (Журнал: E-TWDT-PANIC, 2026-10-03.) */
#if CONFIG_ESP_TASK_WDT_INIT
    {
        esp_task_wdt_config_t wcfg = {
            .timeout_ms     = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
            .idle_core_mask = (1 << 0), /* keep CPU0 watched, drop CPU1 */
#ifdef CONFIG_ESP_TASK_WDT_PANIC
            .trigger_panic  = true,
#else
            .trigger_panic  = false,
#endif
        };
        esp_err_t werr = esp_task_wdt_reconfigure(&wcfg);
        ESP_LOGI(TAG, "TWDT idle watch: CPU0 only: %s", esp_err_to_name(werr));
    }
#endif

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

/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "esp_err.h"
#include "esp_log.h"
#include "string.h"
#include "sys/ioctl.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "driver/ledc.h"
#include "app_video.h"
#include "app_lcd.h"
#include "app_overlay.h"

#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

#ifndef MIN
#define MIN(a, b)               (((a) < (b)) ? (a) : (b))
#endif

static const char *TAG = "app_main";

/* Set one V4L2 control on the video device (esp_video does not validate
 * ctrl_class, so V4L2_CTRL_CLASS_USER works for any mapped control). */
static esp_err_t app_video_set_ctrl(int video_fd, uint32_t v4l2_cid, int32_t value)
{
    struct v4l2_ext_control control = {
        .id    = v4l2_cid,
        .value = value,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER,
        .count      = 1,
        .controls   = &control,
    };

    if (ioctl(video_fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "failed to set v4l2 ctrl 0x%" PRIx32 " to %" PRIi32, v4l2_cid, value);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* OV02C10 capabilities unlocked in this branch: test pattern and frame rate
 * override, both applied before the stream starts. See docs/OV02C10_CAPABILITIES.md */
static void app_camera_apply_runtime_controls(int video_fd)
{
#if CONFIG_EXAMPLE_CAM_TEST_PATTERN > 0
    ESP_LOGI(TAG, "Applying camera test pattern %d", CONFIG_EXAMPLE_CAM_TEST_PATTERN);
    app_video_set_ctrl(video_fd, V4L2_CID_TEST_PATTERN, CONFIG_EXAMPLE_CAM_TEST_PATTERN);
#endif

#if CONFIG_EXAMPLE_CAM_TARGET_FPS > 0
    {
        esp_cam_sensor_format_t sensor_format;

        memset(&sensor_format, 0, sizeof(sensor_format));
        if (ioctl(video_fd, VIDIOC_G_SENSOR_FMT, &sensor_format) != 0 || sensor_format.isp_info == NULL) {
            ESP_LOGW(TAG, "failed to read sensor format, skip fps override");
            return;
        }

        uint32_t pclk    = sensor_format.isp_info->isp_v1_info.pclk;
        uint32_t hts     = sensor_format.isp_info->isp_v1_info.hts;
        uint32_t vts_cur = sensor_format.isp_info->isp_v1_info.vts;
        uint32_t height  = sensor_format.height;
        /* vts_target = ceil(pclk / (fps * hts)), rounded up so fps never exceeds the target */
        uint64_t div     = (uint64_t)CONFIG_EXAMPLE_CAM_TARGET_FPS * hts;
        uint32_t vts_tgt = (uint32_t)((pclk + div - 1) / div);

        if (vts_tgt < vts_cur) {
            ESP_LOGW(TAG, "target fps %d is above the mode default (VTS %lu -> %lu not allowed), keeping default",
                     CONFIG_EXAMPLE_CAM_TARGET_FPS, (unsigned long)vts_cur, (unsigned long)vts_tgt);
        } else if (vts_tgt == vts_cur) {
            ESP_LOGI(TAG, "fps override %d matches the mode default timing",
                     CONFIG_EXAMPLE_CAM_TARGET_FPS);
        } else {
            uint32_t vblank = vts_tgt - height;
            ESP_LOGI(TAG, "fps override %d: VTS %lu -> %lu (vblank %lu), expected fps ~ %.2f",
                     CONFIG_EXAMPLE_CAM_TARGET_FPS,
                     (unsigned long)vts_cur, (unsigned long)vts_tgt, (unsigned long)vblank,
                     (double)pclk / ((double)hts * vts_tgt));
            app_video_set_ctrl(video_fd, V4L2_CID_VBLANK, (int32_t)vblank);
        }
    }
#endif
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len, void *user_data);
static esp_err_t bsp_display_brightness_init(void);
static esp_err_t bsp_display_backlight_on(void);

static esp_lcd_panel_handle_t display_panel;
static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *lcd_buffer[EXAMPLE_LCD_BUF_NUM];

#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE || CONFIG_EXAMPLE_ENABLE_LCD_FPS_OVERLAY
/* Frame rate measurement: counted over a sliding 500 ms window so the
 * on-screen value and the log line both update twice per second. */
static float s_measured_fps;
#endif

void app_main(void)
{
#if CONFIG_EXAMPLE_QUIET_LOGS
    /* Защита от "залипшего" sdkconfig. sdkconfig.defaults применяется только
     * при пересоздании sdkconfig; если проект собран поверх старого
     * диагностического конфига (CONFIG_LOG_DEFAULT_LEVEL_DEBUG), пер-кадровая
     * статистика ISP (~1 КБ текста на кадр) на 115200 бод душит видео-конвейер
     * (замерено: 5-10 fps вместо 30). Принудительно ставим INFO до любой
     * инициализации, чтобы поведение не зависело от состояния sdkconfig.
     * Вернуть отладку: menuconfig -> Example Configuration -> Quiet logs = off. */
    esp_log_level_set("*", ESP_LOG_INFO);
#endif

    // Initialize the LCD
    bsp_display_brightness_init();
    ESP_ERROR_CHECK(app_lcd_init(&display_panel));

    // Initialize the PPA
    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));

    // Initialize the video camera
    esp_err_t ret = app_video_main(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video main init failed with error 0x%x", ret);
        return;
    }

    // Open the video device
    int video_cam_fd0 = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0) {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    // Apply optional sensor runtime controls: test pattern, frame rate override
    app_camera_apply_runtime_controls(video_cam_fd0);

    // Get the LCD frame buffer
#if EXAMPLE_LCD_BUF_NUM == 2
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_panel, 2, &lcd_buffer[0], &lcd_buffer[1]));
#else
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_panel, 3, &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]));
#endif

    // Set the video buffer
#if CONFIG_EXAMPLE_USE_MEMORY_MAPPING
    ESP_LOGI(TAG, "Using map buffer");
    ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, NULL)); // When setting the camera video buffer, it can be written as NULL to automatically allocate the buffer using mapping
#else
    ESP_LOGI(TAG, "Using user defined buffer");
#if CONFIG_CAMERA_SC2336_MIPI_RAW8_1024x600_30FPS
    ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, (void *)lcd_buffer));
#else
    void *camera_buf[EXAMPLE_CAM_BUF_NUM];
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        camera_buf[i] = heap_caps_aligned_calloc(data_cache_line_size, 1, app_video_get_buf_size(), MALLOC_CAP_SPIRAM);
    }
    ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, (void *)camera_buf));
#endif
#endif

    bsp_display_backlight_on();

    // Register the video frame operation callback
    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_video_frame_operation));

    // Start the camera stream task
    ESP_ERROR_CHECK(app_video_stream_task_start(video_cam_fd0, 0, NULL));
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len, void *user_data)
{
    /* Camera-to-LCD geometry, computed once for the actual sensor mode
     * (1288x728, 1920x1080, ...). Works for any resolution the sensor
     * reports, see docs/OV02C10_CAPABILITIES.md */
    static struct {
        uint32_t in_block_w;
        uint32_t in_block_h;
        uint32_t in_off_x;
        uint32_t in_off_y;
        uint32_t out_off_x;
        uint32_t out_off_y;
        float scale;
        bool init;
    } fit = {0};

    if (!fit.init) {
#if CONFIG_EXAMPLE_CAM_DISPLAY_FIT_CROP
        /* Centered crop, 1:1 pixels: sharpest image, full LCD used */
        fit.in_block_w = MIN(camera_buf_hes, EXAMPLE_LCD_H_RES);
        fit.in_block_h = MIN(camera_buf_ves, EXAMPLE_LCD_V_RES);
        fit.in_off_x   = (camera_buf_hes > fit.in_block_w) ? (camera_buf_hes - fit.in_block_w) / 2 : 0;
        fit.in_off_y   = (camera_buf_ves > fit.in_block_h) ? (camera_buf_ves - fit.in_block_h) / 2 : 0;
        fit.scale      = 1.0f;
        fit.out_off_x  = 0;
        fit.out_off_y  = 0;
#else
        /* Scale the whole frame to fit the LCD, aspect ratio preserved.
         * PPA scaling granularity is 1/16, round down so the block never
         * exceeds the LCD (e.g. 1920x1080 -> 960x540, 1288x728 -> 966x546). */
        uint32_t scale_q16;

        fit.in_block_w = camera_buf_hes;
        fit.in_block_h = camera_buf_ves;
        fit.in_off_x   = 0;
        fit.in_off_y   = 0;
        fit.scale      = MIN((float)EXAMPLE_LCD_H_RES / camera_buf_hes,
                             (float)EXAMPLE_LCD_V_RES / camera_buf_ves);
        scale_q16 = (uint32_t)(fit.scale * 16.0f);
        if (scale_q16 == 0) {
            scale_q16 = 1;
        }
        if (scale_q16 > 16) { /* only downscaling is expected here */
            scale_q16 = 16;
        }
        fit.scale     = scale_q16 / 16.0f;
        fit.out_off_x = (EXAMPLE_LCD_H_RES - (uint32_t)(camera_buf_hes * fit.scale)) / 2;
        fit.out_off_y = (EXAMPLE_LCD_V_RES - (uint32_t)(camera_buf_ves * fit.scale)) / 2;
#endif
        fit.init = true;
        ESP_LOGI(TAG, "PPA fit: camera %lux%lu, in block %lux%lu offset (%lu,%lu), scale %.4f, out offset (%lu,%lu)",
                 (unsigned long)camera_buf_hes, (unsigned long)camera_buf_ves,
                 (unsigned long)fit.in_block_w, (unsigned long)fit.in_block_h,
                 (unsigned long)fit.in_off_x, (unsigned long)fit.in_off_y,
                 fit.scale,
                 (unsigned long)fit.out_off_x, (unsigned long)fit.out_off_y);
    }

#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE || CONFIG_EXAMPLE_ENABLE_LCD_FPS_OVERLAY
    {
        static uint32_t frame_counter = 0;
        static int64_t window_start_us = 0;

        if (window_start_us == 0) {
            window_start_us = esp_timer_get_time();
        }
        frame_counter++;

        int64_t now_us = esp_timer_get_time();
        int64_t elapsed_us = now_us - window_start_us;
        if (elapsed_us >= 500000) {  /* update the readout every 500 ms */
            s_measured_fps = (float)frame_counter * 1000000.0f / (float)elapsed_us;
#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE
            ESP_LOGI(TAG, "fps: %.2f, camera_buf_hes: %lu, camera_buf_ves: %lu, camera_buf_len: %d KB",
                     s_measured_fps,
                     (unsigned long)camera_buf_hes, (unsigned long)camera_buf_ves, camera_buf_len / 1024);
#endif
            frame_counter = 0;
            window_start_us = now_us;
        }
    }
#endif
    // ESP_LOGI(TAG,"camera_vedio_frame_operation");

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = camera_buf,
        .in.pic_w = camera_buf_hes,
        .in.pic_h = camera_buf_ves,
        .in.block_w = fit.in_block_w,
        .in.block_h = fit.in_block_h,
        .in.block_offset_x = fit.in_off_x,
        .in.block_offset_y = fit.in_off_y,
        .in.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,
        .out.buffer = lcd_buffer[camera_buf_index],
        .out.buffer_size = ALIGN_UP(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3), data_cache_line_size),
        .out.pic_w = EXAMPLE_LCD_H_RES,
        .out.pic_h = EXAMPLE_LCD_V_RES,
        .out.block_offset_x = fit.out_off_x,
        .out.block_offset_y = fit.out_off_y,
        .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,
#if CONFIG_BOARD_TYPE_JC8012P4A1
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
#elif CONFIG_BOARD_TYPE_JC4880P443
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
#elif CONFIG_BOARD_TYPE_JC1060P470
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
#endif
        .scale_x = fit.scale,
        .scale_y = fit.scale,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config));

#if CONFIG_EXAMPLE_ENABLE_LCD_FPS_OVERLAY
    /* Draw the FPS readout on top of the camera image, then hand the
     * buffer to the panel: draw_bitmap() performs the cache write-back
     * for the full frame, so the overlay pixels get flushed too. */
    app_overlay_draw_fps((uint16_t *)lcd_buffer[camera_buf_index],
                         EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                         s_measured_fps);
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(display_panel, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, lcd_buffer[camera_buf_index]));
}

#define BSP_LCD_BACKLIGHT   GPIO_NUM_23
#define LCD_LEDC_CH         LEDC_CHANNEL_0
static esp_err_t bsp_display_brightness_init(void)
{
    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    ESP_ERROR_CHECK(ledc_timer_config(&LCD_backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&LCD_backlight_channel));
    return ESP_OK;
}

static esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023 * brightness_percent) / 100; // LEDC resolution set to 10bits, thus: 100% = 1023
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));
    return ESP_OK;
}

static esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

static esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}
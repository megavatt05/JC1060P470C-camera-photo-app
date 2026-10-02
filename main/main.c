/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser — standalone Ethernet web browser for JC1060P470C_I_W_Y
 * (ESP32-P4 + JD9165 1024x600 MIPI-DSI + IP101 PHY on the internal EMAC
 * / RMII + touch).
 *
 * This branch is browser-only: the camera subsystem (esp_video, MIPI-CSI,
 * sensor, ISP, PPA) is intentionally absent. app_main boots straight into
 * the browser UI, so a missing or failing camera can never block startup.
 * The full camera app lives in the feature/camos branch.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include "app_lcd.h"
#include "browser/browser.h"

static const char *TAG = "app_main";

static esp_lcd_panel_handle_t display_panel;

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

static esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

void app_main(void)
{
#if CONFIG_EXAMPLE_QUIET_LOGS
    /* Защита от "залипшего" sdkconfig: sdkconfig.defaults применяется только
     * при пересоздании sdkconfig. Принудительно ставим INFO до любой
     * инициализации, чтобы поведение не зависело от состояния sdkconfig.
     * Вернуть отладку: menuconfig -> Example Configuration -> Quiet logs = off. */
    esp_log_level_set("*", ESP_LOG_INFO);
#endif

    /* A brownout reset right after audio starts is the signature of a weak
     * 5V source sagging under the speaker amp load - tell the user up front. */
    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        ESP_LOGW(TAG, "previous reset: BROWNOUT (power sag). The speaker amp "
                      "draws sharp current peaks - use a 5V/2A adapter and a "
                      "short thick USB cable, avoid PC front-panel ports");
    }

    // Initialize the LCD (frame buffers for the browser are cached inside app_lcd)
    ESP_ERROR_CHECK(app_lcd_init(&display_panel));

    ESP_ERROR_CHECK(bsp_display_brightness_init());
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "starting CamBrowser (standalone, no camera)");
    browser_start();
}

set(srcs "main.c" "app_lcd.c" "app_overlay.c"
          "camos/ui.c" "camos/touch.c"
          "net/app_eth.c" "net/log_server.c"
          "browser/web_client.c" "browser/html_text.c" "browser/browser.c"
          "media/media_audio.c" "media/media_player.c" "media/pstats.c"
          "media/sdcard.c")

if(CONFIG_EB_MEDIA_ENABLE)
    list(APPEND srcs "browser/films.c")
endif()

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                       PRIV_REQUIRES
                           esp_timer
                           esp_pm
                           esp_lcd
                           esp_system
                           lwip
                           esp_driver_gpio
                           esp_driver_ledc
                           esp_driver_i2c
                           esp_driver_i2s
                           esp_eth
                           esp_netif
                           esp_event
                           esp_http_client
                           esp_http_server
                           esp-tls
                           heap
                           log
                           fatfs
                           sdmmc
                           esp_driver_sdmmc
                           espressif__esp_codec_dev
                           espressif__esp_player
                           espressif__media_lib_sal
                           espressif__esp_audio_render
                           espressif__esp_video_render
                           espressif__gmf_core
                           espressif__esp_audio_codec
                           espressif__esp_video_codec
)

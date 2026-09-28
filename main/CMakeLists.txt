set(srcs "main.c" "app_video.c" "app_lcd.c")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                       PRIV_REQUIRES
                           esp_timer
                           esp_pm
                           esp_mm
                           esp_lcd
                           esp_driver_gpio
                           esp_driver_spi
                           esp_driver_ledc
                           esp_driver_ppa
                           esp_driver_isp
                           heap
                           log
)

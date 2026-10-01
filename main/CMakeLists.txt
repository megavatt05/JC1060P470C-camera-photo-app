set(srcs "main.c" "app_lcd.c" "app_overlay.c"
          "camos/ui.c" "camos/touch.c"
          "net/app_eth.c"
          "browser/web_client.c" "browser/html_text.c" "browser/browser.c")

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                       PRIV_REQUIRES
                           esp_timer
                           esp_pm
                           esp_lcd
                           esp_driver_gpio
                           esp_driver_ledc
                           esp_driver_i2c
                           esp_eth
                           esp_netif
                           esp_event
                           esp_http_client
                           esp-tls
                           heap
                           log
)

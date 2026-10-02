# CamBrowser — Ethernet-браузер на JC1060P470C_I_W_Y

Ветка `feature/ethernet-browser`: **только браузер, без камеры**. Камерный
стек (esp_video, esp_cam_sensor, esp_ipa, OV02C10, ISP, PPA) вырезан —
он остался в ветке `feature/camos`.

Плата **GUITION JC1060P470C_I_W / _Y** (ESP32-P4 v1.3 + ESP32-C6),
дисплей **JD9165 1024×600** (MIPI-DSI), сеть — **W5500** по SPI, ввод — тач.

| | |
|--|--|
| Дисплей | JD9165, MIPI-DSI, 1024×600 RGB565, подсветка **GPIO23** (LEDC) |
| Ethernet | W5500 SPI: CS=**10**, SCLK=**12**, MOSI=**11**, MISO=**13**, INT=**9**, RST=**14** |
| Тач | I2C port 1: SCL=**GPIO6**, SDA=**GPIO5**; автопробор GT911 / FT5x06 / CST816 |
| Framework | ESP-IDF 6.0.3 |

## Что умеет (фаза 0)

- Загрузка сразу в браузер: камера не инициализируется вообще
- W5500 → esp_eth → esp_netif → DHCP; статус и IP в статус-баре
- Экранная QWERTY-клавиатура, поиск **DuckDuckGo** (по умолчанию) или **Google**
- Парсинг HTML-выдачи в текст, список результатов, чтение страницы со скроллом
- Отрисовка RGB565 прямо во фреймбуфер DPI-панели (двойная буферизация)

Схема соединений, архитектура и дорожная карта:
[docs/ETHERNET_BROWSER.md](docs/ETHERNET_BROWSER.md).

## Сборка

```bash
git clone -b feature/ethernet-browser https://github.com/megavatt05/JC1060P470C-camera-photo-app.git
cd JC1060P470C-camera-photo-app

. $HOME/esp/esp-idf/export.sh   # Windows: export.ps1 / export.bat

idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

> **Обновление существующего клона** (после удаления камерных компонентов):
> обязательно `idf.py fullclean`, затем `idf.py set-target esp32p4 && idf.py build`.
> Старые `build/`, `managed_components/` и `sdkconfig` содержат камерные символы
> и собьются с новым набором компонентов.

## Структура

```
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── components/
│   └── espressif__esp_lcd_jd9365/    # локальная копия (переопределяет registry)
├── main/
│   ├── main.c                        # вход: LCD + подсветка + browser_start()
│   ├── app_lcd.c / .h                # DPI-панель, кэш фреймбуферов, get_fb/flush
│   ├── app_overlay.c / .h            # общий шрифт
│   ├── camos/ui.c                    # RGB565-примитивы (текст, рамки, кнопки)
│   ├── camos/touch.c                 # автопробор тач-контроллера, опрос
│   ├── net/app_eth.c                 # W5500 SPI → esp_eth → esp_netif → DHCP
│   ├── browser/browser.c             # состояния, клавиатура, навигация
│   ├── browser/web_client.c          # HTTPS GET (mbedTLS), cookies, лимит тела
│   ├── browser/html_text.c           # парсеры SERP DDG/Google, html→text
│   └── Kconfig.projbuild             # пины W5500/тача, движок поиска
└── docs/
    ├── ETHERNET_BROWSER.md           # концепция, распиновка, фазы
    └── flash/                        # ГОТОВАЯ ПРОШИВКА: веб-форма, bins, инструкция
```

## Прошивка без сборки (готовые бинарники)

Не нужно ставить ESP-IDF, чтобы залить прошивку: в
[`docs/flash/`](docs/flash/README.md) лежит готовый комплект
(коммит `a779300` — браузер + видео + ФИЛЬМЫ + фиксы):
веб-форма [`web_flasher_cambrowser.html`](https://megavatt05.github.io/JC1060P470C-camera-photo-app/flash/web_flasher_cambrowser.html)
(Chrome/Edge → COM10), единый образ и полная инструкция
[`FLASH_GUIDE_RU.md`](docs/flash/FLASH_GUIDE_RU.md).

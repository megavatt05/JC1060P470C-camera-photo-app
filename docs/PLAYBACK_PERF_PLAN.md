# План: плавное воспроизведение (ESP32-P4)

**Ветка:** `perf/playback-smooth`

## Реализовано (чанк 1)

| Изменение | Зачем |
|-----------|--------|
| demux 6 MB, HTTP 768 KB, prebuffer 2.5 s | Меньше starve на archive.org |
| video_decoder prio 6, stack 12K PSRAM | Больше квантов декоду |
| extractor в PSRAM | Меньше давления на internal RAM |
| тише ESP_GMF_PORT/TASK | Меньше UART overhead |
| convert_movie smooth/max | Контент под realtime P4 |
| лог-подсказка при старте | SD + 320×180 |

## Почему тормозит

HTTP → demux → tinyh264 (SW) → YUV420P → **vid_color_cvt (SW)** → PPA → RGB565.

HW H.264 decode на P4 **нет**. esp_player: 320×240@~68, 640×480@~18.

## Технологии P4

PPA, DMA2D color convert, ISP, JPEG HW, H.264 HW **encode**, H.264 **SW** decode.

## Дальше

- Чанк D: без SW color_cvt
- Чанк F: drop late frames
- UI: подсказка про convert_movie

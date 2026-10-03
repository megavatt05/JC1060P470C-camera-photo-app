# E-PLAYBACK-SMOOTH — тормоза видео на ESP32-P4

**Дата:** 2026-10-03  
**Ветка:** `perf/playback-smooth`  
**Файлы:** `main/media/media_player.c`, `tools/convert_movie.*`

## Причина

1. На P4 **нет HW H.264 decode** — только software (tinyh264 / esp_h264).
2. Espressif: ~320×240 комфортно, ~640×480 ≈ 18 fps.
3. Пайплайн: decode → **SW vid_color_cvt** (YUV420P) → PPA → LCD.
4. Сеть archive.org: TLS reconnect + read timeout → buffering.

## Что сделано (чанк 1)

- Буферы: demux 6 MB, HTTP 768 KB, prebuffer 2.5 s.
- Задачи: video_decoder prio 6, stack 12K в PSRAM; extractor в PSRAM.
- Меньше шума логов GMF на UART (экономия CPU).
- `convert_movie`: профили **smooth** (320×180) и **max** (480×270).
- Подсказка в логе при старте видео.

## Рекомендация

SD + `./tools/convert_movie.sh film.mkv out.mp4 smooth`.  
Стрим 512×288 с archive.org останется на грани.

## Следующие чанки

- D: убрать SW color_cvt при подходящем fourcc.
- F: drop late frames.

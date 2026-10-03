# E-PLAYBACK-SMOOTH — тормоза видео на ESP32-P4

**Дата:** 2026-10-03  
**Ветка:** `perf/playback-smooth`

## Диагноз по логу пользователя

- CPU0 ≈ 17%, CPU1 ≈ 15% — **не** насыщение SW decode.
- Десятки событий `буферизация` / `буфер готов` подряд.
- Ранее: `ESP_GMF_BLOCK: Read timeout`, TLS reconnect archive.org.

**Вывод: узкое место — сеть/CDN, не CPU.**

## Меры (чанк 2)

- demux **8 MB**, HTTP **1 MB**, prebuffer **4 s**
- rebuffer enter 800 ms / resume 2500 ms
- в логе при ≥5 buffering: «скорее сеть/CDN… SD + convert_movie»

## Что делать

1. `git pull` ветки `perf/playback-smooth`, прошивка.
2. Плавная картинка: `tools/convert_movie.sh film.mp4 out.mp4 smooth` → SD.
3. Стрим archive.org 512×288 может оставаться рваным — CDN не realtime.

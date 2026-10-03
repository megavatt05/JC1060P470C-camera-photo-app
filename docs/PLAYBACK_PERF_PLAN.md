# План: плавное воспроизведение на JC1060P470C (ESP32-P4)

**Ветка:** `perf/playback-smooth`  
**База:** `feature/ethernet-browser` @ 47567f32  
**Дата:** 2026-10-03

## Почему тормозит (по логам и коду)

Цепочка сейчас:

```
HTTP(archive.org) → demux/MP4 → tinyh264 (SW) → YUV420P
    → vid_color_cvt (SW!) → PPA → RGB565 1024×600 DPI
```

Из лога:

1. **Сеть** — долгий PREPARING, `ESP_GMF_BLOCK: Read timeout`, TLS reconnect CDN.  
   Даже при demux 4 MB / HTTP 512 KB поток ~500 МБ с archive.org даёт голод буфера.

2. **Нет аппаратного H.264 decode на P4**  
   Официально: HW есть только **encoder** H.264; decoder — software  
   (`esp_h264` / tinyh264 в GMF). 512×288 @23 fps на грани, если ещё и color_cvt на CPU.

3. **Принудительный SW color convert**  
   `VIDEO_RENDER_PIPELINE: Force to use vid_color_cvt for HW not support YUV420P`  
   PPA умеет YUV420, но путь рендера отдаёт **planar YUV420P**, который текущий HW-path
   не принимает → лишний полный проход по кадру на CPU перед PPA.

4. **Масштаб 512×288 → 1024×600** на PPA (это как раз хорошо — HW), но CPU уже занят декодом+cvt.

5. **Фильтр профиля** в `films.c` правильно режет High/Main без constraint — иначе SW-декодер падает ещё сильнее.

## Технологии конверсии / видео на ESP32-P4

| Блок | Что умеет | Для нашего плеера |
|------|-----------|-------------------|
| **PPA** | scale, rotate, mirror, blend; RGB565/888, YUV420/444 | Масштаб на экран, по возможности color без CPU |
| **DMA2D / async color convert** | RGB↔UYVY и копирование окон | Альтернатива тяжёлому SW cvt |
| **ISP** | RAW→RGB/YUV, CCM, AE… | В основном камера, не file playback |
| **JPEG HW encode/decode** | M2M JPEG | Для MJPEG/UVC, не для H.264 фильмов |
| **H.264 HW encode** | до 1080p30 | Запись/стрим с камеры, **не** decode архива |
| **H.264 SW decode** (`esp_h264` / tinyh264) | Baseline / constrained Main | **Единственный** путь для MP4 с archive.org |
| **esp_imgfx / GMF video_color_convert** | SW/ускоренные cvt | Сейчас вынужденный шаг YUV420P→RGB |

**Вывод:** «волшебной» HW-декомпрессии H.264 на P4 нет. Ускорение = меньше работы CPU на кадр + меньше простоев сети + формат, который ест PPA без SW cvt.

## Предлагаемое решение (по чанкам)

### Чанк A — измерение (без смены UX)
- В `pstats`/OSD: фактический FPS рендера, % drop, время decode vs cvt (если API даёт).
- Лог один раз при старте: путь пайплайна (`vid_color_cvt` да/нет).

### Чанк B — сеть (уже частично сделано)
- Оставить крупные буферы; для длинных фильмов — **кнопка «с SD»** (конвертация ПК → fat).
- Опционально: не стартовать PLAY до `prebuffer_resume_ms` с реальным запасом.

### Чанк C — декодер
- Проверить, можно ли вместо/поверх tinyh264 включить **esp_h264 dual-task** decoder в GMF/esp_player.
- Поднять priority `video_decoder`, stack в PSRAM если поддерживается.
- Не держать лишние логи на render core.

### Чанк D — убрать SW color_cvt (главный выигрыш CPU)
- Выяснить fourcc выхода декодера (I420 vs NV12).
- Если PPA/esp_video_render принимает semi-planar — настроить `Video decode dst format` / color_convert так, чтобы **не** было `Force to use vid_color_cvt`.
- Иначе: один HW путь через поддерживаемый формат (документация PPA YUV420).

### Чанк E — контент
- Offline `ffmpeg`: Baseline, ≤480p, CFR 24, bitrate ~1–1.5 Mbps, moov в начале (`+faststart`).
- Расширить UI-подсказку «нужен Baseline…» ссылкой на `tools/convert_movie.*`.

### Чанк F — политика кадров
- При опоздании — drop кадра вместо накопления очереди (если esp_player/GMF даёт флаг).

## Критерий успеха

На ролике 512×288 Baseline с Ethernet или SD:
- OSD FPS стабильно ≥ 20 при заявленных 23;
- нет «пилы» buffering каждые 10 с на SD;
- в логе нет постоянного `Force to use vid_color_cvt` **или** CPU1 не 100% из‑за cvt.

## Не делаем в этой ветке

- Ожидание HW H.264 decode (его нет в silicon/docs).
- Проигрывание High profile без SW, которого нет.

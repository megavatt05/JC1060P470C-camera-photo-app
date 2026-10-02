# fonts/ — шрифты и конвертеры CamBrowser

Инструментальная папка проекта: исходные TTF-шрифты, генератор битмап-глифов
8×8 и обёртка официального LVGL-конвертера. **В сборку прошивки не входит** —
ESP-IDF компилирует только `main/` и `components/`.

## Состав

| Файл | Назначение |
|------|-----------|
| `ttf/DejaVuSansMono.ttf` | источник кириллических глифов (лицензия Bitstream Vera/публичное достояние — свободно распространяется) |
| `ttf/DejaVuSansMono-Bold.ttf` | то же, жирное начертание (запас на будущее) |
| `gen_cyr_font.py` | генератор таблицы 8×8 кириллицы из TTF (Python + Pillow) |
| `cyr_font_table.c` | эталонный вывод генератора — таблица `app_overlay_font8x8_cyr[66][8]` |
| `lvgl_convert.sh` | конвертация TTF → C-шрифт формата LVGL через `npx lv_font_conv` |

## Как это связано с прошивкой

Кириллица на экране рисуется битмап-шрифтом 8×8: таблица
`app_overlay_font8x8_cyr[66][8]` живёт в `main/app_overlay.c` и подобрана так,
чтобы битовая раскладка совпадала со штатной ASCII-таблицей `app_overlay_font8x8`
(bit0 = левый пиксель). Индексы: 0..31 = А..Я, 32..63 = а..я, 64 = Ё, 65 = ё.

## Перегенерация таблицы 8×8

```bash
cd fonts
python3 gen_cyr_font.py            # таблица -> cyr_font_table.c + ASCII-превью
```

Скрипт сначала ищет шрифт в `fonts/ttf/`, затем в системных путях
(`/usr/share/fonts/truetype/dejavu/`), поэтому работает и на свежем клоне.

Дальше скопировать массив `app_overlay_font8x8_cyr` в `main/app_overlay.c`
и собрать прошивку (`idf.py build`). Параметры рендера при необходимости
правятся в `PARAMS` (`size`, `y_shift`, `thr` — порог алиасинга).

Зависимости: `pip install pillow`.

## LVGL-шрифт (векторный, если понадобится)

Для крупного сглаженного текста (LVGL v8/v9) вместо 8×8 битмапа:

```bash
cd fonts
./lvgl_convert.sh ttf/DejaVuSansMono.ttf 16 lv_font_mono_16.c
```

Диапазоны символов в скрипте: ASCII `0x20–0x7F`, `Ё/ё`, `А..я`.
Требуется Node.js/npm — `npx` сам скачает `lv_font_conv` при первом запуске.
Полученный `lv_font_*.c` добавляется в LVGL-проект как обычный `lv_font_t`.

## Добавить другой TTF

Положите файл в `fonts/ttf/` и укажите его первым аргументом
(`gen_cyr_font.py` — правкой `CANDIDATES`, `lvgl_convert.sh` — первым
параметром). Кириллицу поддерживают, например, Liberation Mono/Sans
(`/usr/share/fonts/truetype/liberation/`) и FreeMono/FreeSans
(`/usr/share/fonts/truetype/freefont/`).

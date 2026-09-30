# Документ фиксов: миграция JC1060P470C camera-photo-app на ESP-IDF 6.0.3

> Полная история адаптации компонентов: 9 фиксов + сопутствующие изменения, эволюция
> подходов к патчингу, детальный разбор рантайм-фикса CSI и верификация на железе.
> Документ дополняет [docs/BUILD_IDF6.md](BUILD_IDF6.md) (процедура сборки) и
> [docs/CAMERA_DISPLAY.md](CAMERA_DISPLAY.md) (пайплайн и пины).

**Статус: миграция завершена** — сборка (exit 0) и рантайм (30.14 fps на ESP32-P4 v1.3) подтверждены.

| Параметр | Значение |
|---|---|
| Репозиторий | <https://github.com/megavatt05/JC1060P470C-camera-photo-app> |
| Ветка разработки | `idf603-build-fixes`, слита в `main` (merge-коммит `c76f602`) |
| Коммиты ветки | `b267b43` → `a0d244f` → `50087da` → `12725dc` |
| Референс (IDF 5.5.5) | <https://github.com/megavatt05/ESP32P4-JC1060P470C-I_W_Y/tree/example/camera-photo-app> |
| Версии фреймворка | ESP-IDF 5.5.5 → **6.0.3**, целевой чип `esp32p4` |
| Адаптированные компоненты | esp_video 1.2.0, esp_ipa 1.1.0, esp_lcd_jd9365 1.0.4, esp_cam_sensor 1.2.1 (+OV02C10) |
| Результат | сборка exit 0; рантайм 30.14 fps, 1288×728 RGB565, буфер 1831 KB |

---

## 1. Сводка

Проект «живой preview с камеры OV02C10 на дисплей JD9165» перенесён с ESP-IDF 5.5.5 на
ESP-IDF 6.0.3 без изменения функциональности. Исходный пайплайн данных полностью сохранён:
сенсор отдаёт RAW10 1288×728 @ 30 fps по MIPI-CSI, ISP выполняет demosaic и конверсию в
RGB565, CSI DMA переносит кадры в PSRAM, DPI-контроллер MIPI-DSI выводит их на экран.
Никаких обходных путей «в ущерб качеству» (снижение разрешения, отключение ISP) не потребовалось.

Всего внесено **9 фиксов совместимости** (8 ошибок сборки + 1 рантайм-ошибка) и **пакет
сопутствующих изменений** `sdkconfig` (драйвер flash Boya, отключение PM, смещение таблицы
разделов и др. — раздел 9). Фиксы 1–7 выполнены в копиях компонентов внутри `components/`
(vendoring), фиксы 8–9 — в обычных файлах репозитория. Итоговая сборка воспроизводится
с нуля одной последовательностью `idf.py set-target esp32p4 && idf.py build` без каких-либо
ручных шагов, скриптов или патчей.

Ключевые метрики результата:

| Метрика | Значение |
|---|---|
| Частота кадров (железо, 5+ замеров) | 30.19 → стабильно **30.14 fps** |
| Разрешение / формат буфера | 1288×728, RGB565 |
| Размер кадра | 1831 KB (1288 × 728 × 2 байта) |
| `video_lcd_display.bin` | 499 264 байт (76% раздела `factory` свободно) |
| `bootloader.bin` | 24 272 байта |
| PSRAM / Flash | 32 MB (200 MHz) / 16 MB Boya, QIO |
| Определение сенсора | OV02C10, PID `0x5602` |

## 2. Контекст: проект и железо

**Плата**: GUITION JC1060P470C-I/W/Y — модуль на ESP32-P4 **ревизии v1.3** (+ coprocessor
ESP32-C6 для Wi-Fi/BLE, в данном проекте не задействован). Важно: ревизия чипа ниже v3.0 —
это ключевое обстоятельство рантайм-фикса CSI (раздел 8). На плате установлены 32 MB PSRAM
(HEX, 200 MHz, X16) и 16 MB параллельной flash **Boya** — нестандартный для эталонных
конфигураций Espressif вендор, что потребовало отдельной настройки драйвера (раздел 9).

**Камера**: OV02C10 по MIPI-CSI (2 lane), выход RAW10 в режиме 1288×728 @ 30 fps. Режим
выбран не максимальный из доступных: на 1920×1080 на чипах ревизии < v3.0 возникает
`ISP: fifo overflow` (ограничение фиксируется в `sdkconfig.defaults` комментарием).
Управление сенсором — по SCCB (SDA=GPIO7, SCL=GPIO8).

**Дисплей**: JD9165 1024×600 по MIPI-DSI (2 lane), DPI-режим с 2D-DMA копированием кадров.
Подсветка — GPIO23 (LEDC), питание MIPI — LDO channel 3, 2.5 V.

**Пайплайн данных** (неизменен с версии 5.5.5):

```
OV02C10 ──MIPI-CSI──▶ CSI bridge ──▶ ISP ──▶ CSI DMA ──▶ PSRAM (буфер)
 (RAW10                (bypass            (demosaic,     1288x728x2 B
  1288x728@30)         конверсии)          AWB, RGB565)    = 1831 KB)
                                                            │
JD9165 ◀──MIPI-DSI── DPI panel ◀── DMA2D копирование ◀─────┘
 (1024x600)
```

Приложение (`main/main.c`) в цикле принимает кадры из `esp_video`, считает статистику
(fps, размеры) и выводит их на LCD через `esp_lcd_dpi` API.

## 3. Что изменилось в ESP-IDF 6.0 (и почему проект перестал собираться)

Все возникшие проблемы укладываются в три класса изменений между IDF 5.5.x и 6.0.x.

**Класс 1 — зависимости компонентов.** В IDF 5 мета-компонент `driver` агрегировал
заголовки всех периферийных драйверов: зависимость `driver` автоматически делала видимыми
`driver/gpio.h`, `driver/i2c.h`, `driver/spi_slave.h` и т.д. В IDF 6 мета-компонент
перестал прокидывать include-пути — каждая зависимость от конкретного драйвера должна быть
объявлена в `CMakeLists.txt` компонента явно (`esp_driver_gpio`, `esp_driver_i2c`,
`esp_driver_spi`, `esp_driver_isp`, ...). Сторонние компоненты, написанные под IDF 5
(esp_video 1.2.0, esp_ipa 1.1.0, esp_cam_sensor 1.2.1), этих зависимостей не объявляли —
сборка падала с `fatal error: driver/gpio.h: No such file or directory` и аналогичными.

**Класс 2 — переименования API.** Переименованы поля структур, enum-значения и заголовки,
которые используют компоненты:

| Было (IDF 5.5) | Стало (IDF 6.0) | Где использовалось |
|---|---|---|
| `esp_lcd_dpi_panel_config_t.pixel_format` | `.in_color_format` / `.out_color_format` | `main/app_lcd.c` |
| `.flags.use_dma2d` | функция `esp_lcd_dpi_panel_enable_dma2d()` | `main/app_lcd.c` |
| `CAM_CTLR_COLOR_YUV422` | `CAM_CTLR_COLOR_YUV422_YUYV` (+ UYVY/YVYU/VYUY) | esp_video csi/dvp/spi |
| `COLOR_SPACE_TYPE(x)`, `COLOR_SPACE_RAW` | удалены; цветовые enum — FourCC | esp_video isp device |
| `#include "hal/isp_types.h"` | `#include "driver/isp_types.h"` | esp_ipa |
| `panel_dev_config->color_space` | `->rgb_ele_order` | esp_lcd_jd9365 |

**Класс 3 — поведение CSI-драйвера (рантайм).** В IDF 5.5.5 конверсию цвета всегда выполнял
ISP, а цвета в конфигурации CSI использовались драйвером только для расчёта размеров
буферов. В IDF 6.0 `esp_cam_new_csi_ctlr()` безусловно конфигурирует аппаратную конверсию
формата в CSI bridge — а на ESP32-P4 ревизий ниже v3.0 эта возможность железно отсутствует.
Проявляется это уже после успешной сборки и прошивки, при старте видеопотока — подробному
разбору посвящён раздел 8.

## 4. Эволюция подхода к патчингу

Решение прошло три итерации; финальный подход выбирался по критерию «сборка не должна
зависеть от порядка ручных действий разработчика».

**Этап 1 — точечный патч-скрипт (PowerShell).** Исходно в репозитории существовал
`tools/patch_esp_ipa_idf6.ps1`, заменявший `hal/isp_types.h` → `driver/isp_types.h`. Патч
был необходим, но недостаточен: он не добавлял `REQUIRES esp_driver_isp` в CMakeLists
компонента esp_ipa, из-за чего замена include не имела эффекта — заголовок находился вне
include-path. Кроме того, скрипт требовал Git Bash/PowerShell и не покрывывал остальные
семь фиксов сборки. Именно с этой стадии остались инциденты «собрал — а мне падает на
`hal/isp_types.h`».

**Этап 2 — кроссплатформенный python-патчер.** Разработан
`patch_idf6_managed_components.py`: идемпотентный, с режимом `--check`, строгой
верификацией результата (PATCHED/ALREADY/FAILED + код возврата) и покрытием всех 8 фиксов
сборки в `managed_components/`. Скрипт решал проблему Windows-сборки без Git Bash, но
оставлял системную слабость: его нужно было запускать **после каждого** `idf.py set-target`
или `reconfigure`, заново скачивающего первозданные компоненты из registry. Два реальных
инцидента подряд (пропуск шага / другой порядок команд → в `esp_ipa_types.h` снова
оказывался `hal/isp_types.h`) показали, что решение «ручной шаг в процессе сборки»
принципиально ненадёжно.

**Этап 3 — vendoring (финальное решение).** Пропатченные копии esp_video 1.2.0,
esp_ipa 1.1.0 и esp_lcd_jd9365 1.0.4 перенесены из `managed_components/` в `components/`
репозитория — по той же конвенции, что уже использовалась для esp_cam_sensor (+OV02C10).
Система сборки ESP-IDF даёт локальному `components/` приоритет над registry: менеджер
компонентов не скачивает эти пакеты заново, а `dependencies.lock` переписывает их на
`source.type = local`. Ручные шаги исчезли полностью: `clone → set-target → build`.

| Критерий | Этап 1 (PS1) | Этап 2 (python) | Этап 3 (vendoring) |
|---|---|---|---|
| Покрытие фиксов | 1 из 8 | 8 из 8 | все, «зашиты» в исходники |
| Платформы | Windows (PS/Git Bash) | любые (python) | любые |
| Ручной шаг после set-target | нужен | нужен | **не нужен** |
| Зависимость от порядка команд | высокая | средняя | **нет** |
| Обновляемость компонентов | registry | registry | вручную (заморожено) |
| Цена | — | — | +61 330 строк в репо (336 файлов) |

Урок, зафиксированный в истории ветки: **любой обязательный ручной шаг в процессе сборки —
это отложенный инцидент**. Дублирование трёх компонентов (≈4 MB исходников) — приемлемая
цена за детерминированную сборку, тем более что обновление этих компонентов и так было
невозможно без ручной синхронизации с upstream (см. раздел 12 о переходе на esp_video 2.x).

## 5. Архитектура решения: vendoring компонентов

Механика приоритета в системе сборки ESP-IDF: при разрешении зависимостей компонент
с искомым именем сначала ищется в локальном каталоге `components/` проекта, и только
затем — в Component Registry. Таким образом, присутствие
`components/espressif__esp_video/` делает запись `esp_video: version ~1.2.0` в
`main/idf_component.yml` фактически бездействующей: менеджер не скачивает пакет из
registry, а `dependencies.lock` после первого `set-target` переписывается на
`source.type = local`. Состояние `managed_components/` (первозданные пакеты, следы
предыдущих экспериментов) перестаёт иметь значение — устаревшие каталоги игнорируются.

Итоговый состав `components/`:

| Компонент | Версия | Почему vendored | Фиксы |
|---|---|---|---|
| `espressif__esp_cam_sensor` | 1.2.1 + драйвер OV02C10 | OV02C10 отсутствует в registry-версии | Ф8 |
| `espressif__esp_video` | 1.2.0 | не собирается на IDF 6 | Ф3, Ф4, Ф5, Ф7 |
| `espressif__esp_ipa` | 1.1.0 | не собирается на IDF 6 | Ф1, Ф2 |
| `espressif__esp_lcd_jd9365` | 1.0.4 | не собирается на IDF 6 | Ф6 |

Конвенция копирования повторяет подход esp_cam_sensor: служебный `.component_hash`
сохранён, `CHECKSUMS.json` удалён, содержимое идентично выкачанному из registry пакету
той же версии **плюс применённые фиксы**. Версии зафиксированы: целенаправленно не
использовались esp_video 2.x / esp_cam_sensor 2.6+, потому что они требуют
esp_cam_sensor 2.6, в котором нет OV02C10 (см. раздел 12).

Git-история ветки `idf603-build-fixes` (слита в `main`, merge `c76f602`):

```
*   c76f602  Merge branch 'idf603-build-fixes': миграция на ESP-IDF 6.0.3 завершена
|\
| * 12725dc  fix(runtime): CSI не стартует на IDF 6 — конверсия формата запрещена на чипах < v3.0
| * 50087da  feat: патченные компоненты в components/ — сборка без скриптов и патчей   (336 файлов, +61330)
| * a0d244f  fix: кроссплатформенный патч-скрипт managed_components (Windows без Git Bash)
| * b267b43  fix: полная сборка на ESP-IDF 6.0.3 (esp32p4) — верифицировано с нуля
|/
* 185b088  feat: phase4 integration — OV02C10 sdkconfig, stats, BUILD_IDF6 guide   ← точка ветвления
```

## 6. Реестр фиксов: зависимости CMake

Сводная таблица всех изменений кода (нумерация соответствует `docs/BUILD_IDF6.md`):

| # | Компонент / файл | Суть | Класс |
|---|---|---|---|
| Ф1 | esp_ipa `esp_ipa_types.h` | `hal/isp_types.h` → `driver/isp_types.h` | API |
| Ф2 | esp_ipa `CMakeLists.txt` | + `REQUIRES esp_driver_isp` | CMake |
| Ф3 | esp_video `CMakeLists.txt` | + `priv_requires`: `esp_driver_gpio`, `esp_driver_i2c` | CMake |
| Ф4 | esp_video `esp_video_isp_device.c` | YUV422 → 4 case; `COLOR_SPACE_TYPE_IS_RAW` | API |
| Ф5 | esp_video csi/dvp/spi device | `CAM_CTLR_COLOR_YUV422` → `..._YUYV` | API |
| Ф6 | esp_lcd_jd9365 `esp_lcd_jd9365.c` | `color_space` → `rgb_ele_order` | API |
| Ф7 | esp_video `esp_video_csi_device.c` | вход CSI = выход на IDF ≥ 6 (**рантайм**) | поведение |
| Ф8 | esp_cam_sensor `CMakeLists.txt` | + `esp_driver_spi` в requires | CMake |
| Ф9 | `main/app_lcd.c` | `pixel_format` → in/out; `dma2d` → функция | API |

### Ф2. esp_ipa: REQUIRES esp_driver_isp

**Симптом:** `fatal error: driver/isp_types.h: No such file or directory` — возникает,
если применён только Ф1 (замена include) без объявления зависимости.

**Причина:** в IDF 6 заголовок `driver/isp_types.h` принадлежит компоненту
`esp_driver_isp`; без строки `REQUIRES` его include-path не виден компоненту esp_ipa.
Именно связка Ф1+Ф2 (а не одна замена include) составляет корректный фикс — источник
двух реальных инцидентов на этапе 1–2 (см. раздел 4).

```c
// components/espressif__esp_ipa/CMakeLists.txt — ДО:
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include)
// ПОСЛЕ:
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       REQUIRES esp_driver_isp)
```

### Ф3. esp_video: priv_requires esp_driver_gpio, esp_driver_i2c

**Симптом:** `fatal error: driver/gpio.h: No such file or directory` (далее `driver/i2c.h`,
`driver/spi_slave.h` и др. — по мере появления в единицах трансляции).

**Причина:** класс 1 из раздела 3 — мета-компонент `driver` из `priv_requires` в IDF 6
больше не раскрывается в заголовки конкретных драйверов.

```cmake
# components/espressif__esp_video/CMakeLists.txt — ДО:
set(priv_requires "vfs")
# ПОСЛЕ:
set(priv_requires "vfs" "esp_driver_gpio" "esp_driver_i2c")
```

### Ф8. esp_cam_sensor: esp_driver_spi в requires

**Симптом:** `fatal error: driver/spi_slave.h: No such file or directory` при сборке
esp_cam_sensor.

**Причина:** публичный заголовок `esp_cam_ctlr_spi.h` инклудит `driver/spi_slave.h`;
в IDF 6 зависимость должна быть явной. Особенность фикса: здесь меняется **публичный**
`requires` (а не `priv_requires`), потому что заголовок входит в `include/` компонента.

```cmake
# components/espressif__esp_cam_sensor/CMakeLists.txt — ДО:
set(requires "driver" "esp_sccb_intf" "esp_driver_cam")
# ПОСЛЕ (с комментарием в исходнике):
# ESP-IDF 6.0: публичный заголовок esp_cam_ctlr_spi.h инклудит driver/spi_slave.h —
# в IDF 6 мета-компонент driver больше не прокидывает инклуды esp_driver_*, добавляем явно
set(requires "driver" "esp_sccb_intf" "esp_driver_cam" "esp_driver_spi")
```

## 7. Реестр фиксов: переименования API

### Ф1. esp_ipa: hal/isp_types.h → driver/isp_types.h

**Симптом (этап 1, старый PS1-скрипт):** `fatal error: hal/isp_types.h: No such file or
directory` — заголовок физически перемещён из HAL в драйвер.

```c
// components/espressif__esp_ipa/include/esp_ipa_types.h:12
// ДО:  #include "hal/isp_types.h"
// ПОСЛЕ:
#include "driver/isp_types.h"
```

### Ф4. esp_video (ISP-девайс): YUV422-кейсы и COLOR_SPACE_TYPE

Два независимых переименования в `esp_video_isp_device.c`.

**(а)** Единый enum `CAM_CTLR_COLOR_YUV422` разбит в IDF 6 на четыре значения с явным
порядком байтов; маппинг в `ISP_COLOR_YUV422` теперь собирается из четырёх case:

```c
// ДО:
case CAM_CTLR_COLOR_YUV422:
    *isp_color = ISP_COLOR_YUV422;
    break;
// ПОСЛЕ (esp_video_isp_device.c:417):
case CAM_CTLR_COLOR_YUV422_YUYV:
case CAM_CTLR_COLOR_YUV422_UYVY:
case CAM_CTLR_COLOR_YUV422_YVYU:
case CAM_CTLR_COLOR_YUV422_VYUY:
    *isp_color = ISP_COLOR_YUV422;
    break;
```

**(б)** Макросы `COLOR_SPACE_TYPE()` и `COLOR_SPACE_RAW` удалены в IDF 6 (цветовые
enum-ы стали FourCC-значениями). Вместо обращения к удалённому API введён локальный
макрос, распознающий Bayer-RAW по префиксу FourCC `'R','A','W'`:

```c
// ДО (использование):
if ((COLOR_SPACE_TYPE(color) == COLOR_SPACE_RAW) && ...)
// ПОСЛЕ — локальный макрос (esp_video_isp_device.c:31):
/*
 * ESP-IDF 6.0: макросы COLOR_SPACE_TYPE и COLOR_SPACE_RAW удалены.
 * Цветовые enum'ы теперь FourCC; Bayer-RAW начинается с байтов 'R','A','W'.
 */
#define COLOR_SPACE_TYPE_IS_RAW(color) \
    (((uint32_t)(color) & 0x00FFFFFF) == ((uint32_t)ESP_COLOR_FOURCC('R', 'A', 'W', 0) & 0x00FFFFFF))
// использование:
if ((COLOR_SPACE_TYPE_IS_RAW(isp_in_color)) && (!COLOR_SPACE_TYPE_IS_RAW(isp_out_color))) {
    isp_video->af_support = 1;   // AF работает только на RAW-входе
}
```

### Ф5. esp_video (csi/dvp/spi device): CAM_CTLR_COLOR_YUV422_YUYV

**Симптом:** ошибка компиляции `CAM_CTLR_COLOR_YUV422 undeclared`.

Точки замены (все — выбор цвета контроллера для формата V4L2 `PIX_FMT_YUV422P`-семейства):
`esp_video_csi_device.c:104`, `esp_video_csi_device.c:146`, `esp_video_dvp_device.c:70`,
`esp_video_spi_device.c:54`.

```c
// ДО:  *csi_color = CAM_CTLR_COLOR_YUV422;
// ПОСЛЕ:
*csi_color = CAM_CTLR_COLOR_YUV422_YUYV;
```

### Ф6. esp_lcd_jd9365: color_space → rgb_ele_order

**Симптом:** `error: 'esp_lcd_panel_dev_config_t' has no member named 'color_space'`.

**Причина:** поле структуры конфигурации панели переименовано; порядок субпикселей
теперь задаётся `rgb_ele_order` (реализуется LCD-командой `36h`, MADCTL).

```c
// components/espressif__esp_lcd_jd9365/esp_lcd_jd9365.c:81 — ДО:
switch (panel_dev_config->color_space) {
// ПОСЛЕ:
switch (panel_dev_config->rgb_ele_order) {
case LCD_RGB_ELEMENT_ORDER_RGB:
    jd9365->madctl_val = 0;
    break;
case LCD_RGB_ELEMENT_ORDER_BGR:
    jd9365->madctl_val |= LCD_CMD_BGR_BIT;
    break;
```

Примечание: в парном компоненте `esp_lcd_jd9165` (драйвер дисплея этой платы) код уже
был корректен — патчился только jd9365.

### Ф9. main/app_lcd.c: pixel_format и DMA2D

**Симптом:** `error: 'esp_lcd_dpi_panel_config_t' has no member named 'pixel_format'`;
затем — `no member named 'use_dma2d'`.

**Решение** (в трёх конфигурациях плат — JC1060P470, JC4880P443 и старшей):

```c
// ДО:
.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
...
.flags.use_dma2d = true,
// ПОСЛЕ:
.in_color_format  = LCD_COLOR_FMT_RGB565,
.out_color_format = LCD_COLOR_FMT_RGB565,
...
#if ESP_IDF_VERSION_MAJOR < 6
        .flags.use_dma2d = true,
#endif
```

и после `esp_lcd_panel_init()` (включение 2D-DMA вынесено в отдельную функцию):

```c
#if ESP_IDF_VERSION_MAJOR >= 6
    /* ESP-IDF 6.0: флаг .flags.use_dma2d удалён — DMA2D включается функцией после init */
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(display_handle));
#endif
```

В начало файла добавлен `#include "esp_idf_version.h"`. Условная компиляция сохраняет
собираемость на IDF 5.5.x (заявленная совместимость проекта с обеими ветками фреймворка).

## 8. Фикс 7 (рантайм): CSI не стартует — конверсия формата запрещена на чипах < v3.0

Единственный фикс, который не могла обнаружить компиляция: все восемь предыдущих
устраняли ошибки сборки, этот проявился на собранной и прошитой плате.

### 8.1. Симптом

Прошивка грузится, сенсор детектируется, панели и LCD инициализируются — но при старте
видеопотока (`app_video: Video Stream Start`) вывод останавливается, кадры не приходят:

```
I (1848) ov02c10: Detected Camera sensor PID=0x5602
I (2057) app_video: width=1288 height=728
I (2121) app_video: Video Stream Start
E (2122) CSI: esp_cam_new_csi_ctlr(227): failed to configure format conversion
E (2122) csi_video: video->ops->start=106
```

Код возврата `106` — это `0x106` = `ESP_ERR_NOT_SUPPORTED` (esp_video печатает его без
префикса `0x`). Стек вызовов: `app_main` → `esp_video_start` → `csi_video_start` →
`esp_cam_new_csi_ctlr` (драйвер IDF) → ошибка на строке 227.

### 8.2. Диагностика: трассировка до исходников IDF 6.0.3

Число в скобках — номер строки `esp_cam_ctlr_csi.c`, он же — место ошибки
(`components/esp_driver_cam/csi/src/esp_cam_ctlr_csi.c`):

```c
// esp_cam_new_csi_ctlr(), строки 221–227 (IDF 6.0.3):
mipi_csi_brg_ll_enable_color_conversion(ctlr->hal.bridge_dev, true);

cam_ctlr_format_conv_config_t format_conv_config = {
    .src_format = config->input_data_color_type,
    .dst_format = config->output_data_color_type,
};
ESP_GOTO_ON_ERROR(s_csi_ctlr_format_conversion(&(ctlr->base), &format_conv_config),
                   err, TAG, "failed to configure format conversion");   // ← строка 227
```

Конфигурацию передаёт esp_video 1.2.0: на вход CSI — цвет сенсора (`in_color` = RAW10),
на выход — запрошенный приложением формат (`out_color` = RGB565). Дальше:

```c
// s_csi_ctlr_format_conversion(), строки 701–729 (IDF 6.0.3):
static esp_err_t s_csi_ctlr_format_conversion(esp_cam_ctlr_t *handle,
                                               const cam_ctlr_format_conv_config_t *config)
{
    ...
    if (ctlr->custom_data_depth || config->src_format == config->dst_format) {
        mipi_csi_brg_ll_set_color_mode_bypass(ctlr->hal.bridge_dev, true);   // bypass — OK
        return ESP_OK;
    } else {
#if CONFIG_IDF_TARGET_ESP32P4
        //If ESP32P4 chip version is less than v3.0, not support color format conversion
        unsigned chip_version = efuse_hal_chip_revision();
        if (!ESP_CHIP_REV_ABOVE(chip_version, 300)) {
            return ESP_ERR_NOT_SUPPORTED;                                    // ← наша ветка
        }
#endif
        if (!s_is_color_format_conversion_supported(config->src_format) ||
            !s_is_color_format_conversion_supported(config->dst_format)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        ...
    }
}
```

Цепочка отказов на плате JC1060P470C (чип v1.3):

1. `src_format (RAW10) != dst_format (RGB565)` → ветка bypass не выбирается;
2. `efuse_hal_chip_revision()` = 1.3, `ESP_CHIP_REV_ABOVE(300)` = ложь → жёсткий
   `ESP_ERR_NOT_SUPPORTED` — на ревизиях ниже v3.0 аппаратная конверсия в CSI bridge
   физически отсутствует;
3. даже на чипе v3.0+ той же конфигурации не поможет: список
   `s_is_color_format_conversion_supported()` включает только RGB888/RGB565/YUV420/
   YUV422-варианты — **RAW-форматов в нём нет вовсе**. Иными словами, конфигурация
   «RAW10 на входе, RGB565 на выходе», которую esp_video 1.2.0 отправляет в IDF 6,
   не работает ни на одной ревизии ESP32-P4.

### 8.3. Почему на IDF 5.5.5 это работало

В IDF 5.5.5 функция `s_csi_ctlr_format_conversion` — заглушка с комментарием
«not supported yet», и **никогда не вызывалась** из `esp_cam_new_csi_ctlr`. Цвета
`input/output_data_color_type` использовались драйвером только для расчёта bpp и размеров
DMA-буферов. Реальную конверсию RAW10 → RGB565 всегда выполнял ISP, который и так стоит
в тракте данных между CSI и памятью. То есть конфигурация esp_video была корректной —
изменилось поведение драйвера, который внезапно начал пытаться конфигурировать
несуществующую в железе конверсию.

### 8.4. Эталонное решение: esp_video 2.x

Официальный espressif/esp_video 2.x (рассмотрен 2.5.0 из Component Registry) содержит
явное ветвление в `esp_video_csi_format.c` для случая «Old chip: CSI cannot convert»:
на IDF ≥ 6.0 входной формат CSI объявляется равным запрошенному выходному, а обязанность
выдать нужный формат ложится на ISP (RGB565 присутствует в списке его выходных форматов).
На IDF 5.x, где конверсию в драйвере никто не вызывает, сохраняется прежнее поведение.
Именно эта логика и воспроизведена в фиксе — минимально, без портирования всего esp_video 2.x
(что потребовало бы esp_cam_sensor 2.6 без OV02C10).

### 8.5. Решение

`components/espressif__esp_video/src/device/esp_video_csi_device.c`, `csi_video_start()`:

```c
// ДО (все версии IDF):
.input_data_color_type  = csi_video->state.in_color,    // RAW10 — сенсор
.output_data_color_type = csi_video->state.out_color,   // RGB565 — приложение

// ПОСЛЕ:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    /* ESP-IDF 6.0: аппаратная конверсия цвета в CSI bridge недоступна на
     * ESP32-P4 ревизий ниже v3.0 — esp_cam_new_csi_ctlr() возвращает
     * ESP_ERR_NOT_SUPPORTED из s_csi_ctlr_format_conversion().
     * CSI DMA принимает выход ISP, поэтому на IDF 6 входной цвет CSI
     * объявляем равным выходному — драйвер выбирает режим bypass.
     * Аналогично espressif/esp_video 2.x (esp_video_csi_format.c,
     * ветка "Old chip: CSI cannot convert"). */
    .input_data_color_type = csi_video->state.out_color,
#else
    .input_data_color_type = csi_video->state.in_color,
#endif
    .output_data_color_type = csi_video->state.out_color,
```

В файл добавлен `#include "esp_idf_version.h"`.

**Почему это безопасно.** Теперь `src_format == dst_format` (оба RGB565), и драйвер
выбирает ветку `mipi_csi_brg_ll_set_color_mode_bypass(true)` — ту же, что выбрана
механизмом bypass на чипах v3.0+. Через CSI bridge данные и так проходят уже
преобразованными: ISP стоит раньше в тракте и выводит RGB565. Размеры буферов не
меняются — bpp выхода остаётся прежним. Для режима `bypass_isp = true` (ISP отключён,
`out_color == in_color` и раньше) фикс не меняет ничего. Конвейер в целом не изменился:

```
сенсор RAW10 → ISP (demosaic + AWB + RGB565) → CSI DMA (bypass) → буфер 1831 KB → DMA2D → LCD
```

### 8.6. Верификация

Подтверждено на железе (ESP32-P4 v1.3, прошивка `12725dc`) — полный лог и метрики в
разделе 10: поток стартует без ошибок, 30.14 fps, буфер 1288×728×2 байта. Единственное
оставшееся предупреждение — `ISP_AWB: Subwindow feature is not supported on REV < 3.0` —
ожидаемое ограничение ревизии чипа (AWB продолжает работать по полному кадру) и
наблюдалось также на эталонной сборке 5.5.5.

## 9. Сопутствующие изменения

Помимо девяти фиксов кода, миграция потребовала настройки `sdkconfig` и сопровождалась
предысторией в `main` (коммиты до точки ветвления `185b088`). Полный контекст ниже.

### 9.1. Предыстория в main (до ветки)

| Коммит | Суть |
|---|---|
| `594c46c` | resolve esp_ipa conflict — drop pin, disable ISP pipeline controller |
| `057f276` | CMake include paths + script to patch `esp_ipa_types.h` для IDF 6 (PS1-скрипт этапа 1) |
| `712d8cb` | disable PM_ENABLE — `pmu_sleep`/`efuse_ll` сломаны на P4 rev < 3.0 в IDF 6.0.3 |
| `2f91418` | план драйвера OV02C10 под IDF 6 |
| `185b088` | phase4: OV02C10 sdkconfig, статистика, BUILD_IDF6 — точка ветвления |

### 9.2. Ключевые строки sdkconfig.defaults

| Параметр | Значение | Причина |
|---|---|---|
| `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` | `y` | ревизия чипа платы — v1.3 |
| `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP` | `y` | на платах стоит flash Boya; без опции boot-лог сообщает «Detected boya flash chip but using generic driver». С опцией — чистое `detected chip: boya` |
| `CONFIG_PARTITION_TABLE_OFFSET` | `0x10000` | bootloader IDF 6 больше; первая запись `partitions.csv` — `0x11000` |
| `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS` | `y` | в IDF 6 warnings по умолчанию — errors; сторонние компоненты (esp_video 1.2) иначе не собираются |
| `CONFIG_PM_ENABLE` | not set | `pmu_sleep`/`efuse_ll` сломаны на P4 rev < 3.0 в IDF 6.0.3 (коммит `712d8cb`) |
| `CONFIG_CAMERA_OV02C10_MIPI_RAW10_1288x728_30FPS` | `y` | на 1920×1080 на rev < v3.0 — `ISP: fifo overflow` |
| `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`, `CONFIG_IDF_EXPERIMENTAL_FEATURES` | `y` | ISP-контроллер тракта esp_video |
| зависимость `espressif/usb` в `main/idf_component.yml` | — | USB Serial/JTAG на P4 в IDF 6 |

Фикс Boya включён в ветку вместе с рантайм-фиксом (коммит `12725dc`): boot-лог
пользователя явно просил включить родной драйвер. Цена — 80 байт кода (499 184 → 499 264).

## 10. Верификация

### 10.1. Сборка

Все сценарии проверены реальными прогонами (не «должно работать», а exit 0):

| Сценарий | Эпоха | Результат |
|---|---|---|
| Чистая сборка с нуля: `rm -rf build managed_components sdkconfig` → `set-target` → `build` | b267b43 | exit 0, 499 232 B |
| «Спасение» дерева с первозданными `managed_components`: патч-скрипт → `build` | a0d244f | exit 0 |
| Повторный запуск патчера (идемпотентность) | a0d244f | 0 исправлено / 8 ALREADY, exit 0 |
| Windows без Git Bash: `python patch_idf6_managed_components.py` → `build` | a0d244f | exit 0 |
| Свежий клон с GitHub → `set-target` → `build`, без патчей | 50087da | exit 0, 499 184 B |
| Финальная чистая сборка (после Boya) | 12725dc | exit 0, **499 264 B** |

Эволюция размера `video_lcd_display.bin` объяснима и не содержит сюрпризов:

| Коммит | Бинарь, B | Причина изменения |
|---|---|---|
| `b267b43` | 499 232 | база (компоненты из `managed_components/`) |
| `50087da` | 499 184 | −48 B: пути `./components/` короче `./managed_components/` в отладочных макросах; функционально идентично |
| `12725dc` | 499 264 | +80 B: драйвер flash Boya |

Bootloader во всех сценариях — 24 272 B. Раздел `factory` (2 MB) заполнен на 24%.

### 10.2. Рантайм на железе

Плата ESP32-P4 v1.3, прошивка `12725dc`, монитор 115200. Фрагмент лога:

```
I (186) boot: Loaded app from partition at offset 0x20000
I (1500) spi_flash: detected chip: boya
I (1503) spi_flash: flash io: qio
I (1493) esp_psram: Adding pool of 32768K of PSRAM memory to heap allocator
I (1537) app_lcd: Install panel IO
I (1537) jd9165: version: 2.0.2
I (1848) ov02c10: Detected Camera sensor PID=0x5602
I (2057) app_video: driver: MIPI-CSI, width=1288 height=728
I (2063) app_main: Using user defined buffer
I (2121) app_video: Video Stream Start
W (2122) ISP_AWB: Subwindow feature is not supported on REV < 3.0, subwindow will not be configured
I (3784) app_main: fps: 30.188661
I (3785) app_main: camera_buf_hes: 1288, camera_buf_ves: 728, camera_buf_len: 1831 KB
I (5443) app_main: fps: 30.142744
I (7102) app_main: fps: 30.142562
I (8761) app_main: fps: 30.142671
I (10419) app_main: fps: 30.142817
```

Контрольные точки: ошибка `esp_cam_new_csi_ctlr(227)` отсутствует; fps стабилен (разброс
< 0.1% на пяти замерах); `camera_buf_len` = 1831 KB = 1288 × 728 × 2 — кадр
действительно RGB565; flash определяется как boya без предупреждений; PSRAM 32 MB
подключена. Warning `ISP_AWB` — норма для ревизии v1.3 (см. 8.6).

## 11. Процедура сборки и обновления клона

### 11.1. Чистая сборка

```bash
git clone https://github.com/megavatt05/JC1060P470C-camera-photo-app.git
cd JC1060P470C-camera-photo-app

. $HOME/esp/esp-idf/export.sh        # Linux; Windows: export.ps1 или IDF-терминал

idf.py set-target esp32p4
idf.py build
idf.py -p COM10 flash monitor
```

Никаких патчей и скриптов: компоненты из `components/` подхватываются автоматически,
`dependencies.lock` переписывается на локальные источники при первом `set-target`.

### 11.2. Обновление существующего клона

После `git pull` (особенно при переходе с версий до `50087da`, когда компоненты ещё
патчились скриптом) **обязателен** перегенерирующий шаг:

```bash
git pull
idf.py set-target esp32p4     # обязательно; иначе build-каталог помнит старые пути
idf.py build
```

Эквивалентная альтернатива — полная очистка перед сборкой:
`rm -rf build managed_components sdkconfig`. Сценарий «pull без set-target/очистки»
проверен и падает с ошибкой разрешения зависимостей; после `set-target` та же
рабочая копия собирается успешно (раздел 10.1).

### 11.3. Ожидаемый результат

Лог прошивки и контрольные точки — в разделе 10.2: сенсор PID `0x5602`, поток стартует
без ошибок CSI, ~30 fps, буфер 1831 KB.

## 12. Выводы и рекомендации

### 12.1. Итоги

Миграция завершена без потери функциональности: пайплайн, разрешение и частота кадров
идентичны эталонной сборке на IDF 5.5.5. Все проблемы укладываются в три класса
изменений IDF 6 (зависимости, переименования, поведение CSI-драйвера) и закрываются
малыми по объёму правками — суммарно 42 строки в рантайм-фиксе и точечные замены в
остальных. Архитектурный вывод, подтверждённый двумя реальными инцидентами: любой
обязательный ручной шаг в процессе сборки — отложенный сбой; vendoring устранил этот
класс проблем целиком.

### 12.2. Технический долг и путь модернизации

Vendored-компоненты заморожены на версиях 1.2.0 / 1.1.0 / 1.0.4 — обновления upstream
не подтягиваются автоматически. Это осознанное решение: альтернативы, совместимой с
OV02C10, в registry нет. Целевое состояние: после появления OV02C10 в апстриме
`esp_cam_sensor` 2.6+ перейти на `esp_video` 2.x (нативная поддержка IDF 6, включая
обход конверсии CSI — логика фикса Ф7 там уже есть), удалить из `components/` три
компонента и снять фиксы Ф1–Ф7. До этого момента обновление возможно только ручной
синхронизацией с сопоставлением изменений.

### 12.3. Гигиена доступа

GitHub PAT, использованный для публикации веток, передавался по открытому каналу — после
завершения работ его следует отозвать или ротировать.

### 12.4. Ссылки

| Ресурс | URL |
|---|---|
| Репозиторий (main, после слияния) | <https://github.com/megavatt05/JC1060P470C-camera-photo-app> |
| Ветка разработки (сохранена как архив) | `idf603-build-fixes` в том же репозитории |
| Референс, IDF 5.5.5 | <https://github.com/megavatt05/ESP32P4-JC1060P470C-I_W_Y/tree/example/camera-photo-app> |
| ESP-IDF v6.0.3 | <https://github.com/espressif/esp-idf/tree/v6.0.3> |
| Процедура сборки | [docs/BUILD_IDF6.md](BUILD_IDF6.md) |
| Пайплайн и пины | [docs/CAMERA_DISPLAY.md](CAMERA_DISPLAY.md) |
| Этот документ | [docs/FIXES_IDF603.md](FIXES_IDF603.md) |


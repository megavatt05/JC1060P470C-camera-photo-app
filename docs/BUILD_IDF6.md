# Сборка JC1060P470C-camera-photo-app (ESP-IDF 6.0.3, ветка main)

## ESP-IDF 6.0.3 — сборка без патчей

Все компоненты, требовавшие адаптации под IDF 6, **уже лежат в `components/`
репозитория в исправленном виде** (по тому же принципу, по которому в проекте
давно живёт `espressif__esp_cam_sensor` с драйвером OV02C10). Компонент-менеджер
видит локальные копии и **не скачивает** эти компоненты из registry —
`managed_components/` после `set-target` содержит только нетронутые компоненты.

Ничего патчить, запускать скрипты или чистить не нужно.

> Полная история адаптации, детальный разбор каждого из 9 фиксов (с кодом до/после)
> и рантайм-фикса CSI: [docs/FIXES_IDF603.md](FIXES_IDF603.md).

**Windows (ESP-IDF PowerShell / CMD):**

```powershell
git clone https://github.com/megavatt05/JC1060P470C-camera-photo-app.git
cd JC1060P470C-camera-photo-app
. C:\esp\v6.0.3\esp-idf\export.ps1
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

**Linux / macOS:**

```bash
git clone https://github.com/megavatt05/JC1060P470C-camera-photo-app.git
cd JC1060P470C-camera-photo-app
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

### Обновление существующего клона (git pull)

Если клон уже существует (например, с прошлых экспериментов со скриптами
`patch_esp_ipa_idf6.ps1` / `patch_idf6_managed_components.py`):

```bash
git checkout main
git pull
rm -rf build managed_components sdkconfig     # переключиться на components/
idf.py set-target esp32p4
idf.py build
```

`rm -rf managed_components` важен: в старом каталоге могли остаться наполовину
пропатченные компоненты, а конфигурация build/ помнит старые пути. После
`set-target` проект собирается из `components/` (проверено; очистка нужна для
полной детерминированности).

Ожидается: `PID=0x5602`, `width=1288 height=728`, `fps: ~30`.
Обязательно: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, 1288x728, PM off.

## Что именно исправлено (зафиксировано в components/)

| # | Компонент | Исправление |
|---|-----------|-------------|
| 1 | esp_ipa | `hal/isp_types.h` → `driver/isp_types.h` (hal-заголовок удалён в IDF 6) |
| 2 | esp_ipa | + `REQUIRES esp_driver_isp` в CMakeLists (иначе п.1 не работает — нет include-пути) |
| 3 | esp_video | + `esp_driver_gpio`, `esp_driver_i2c` в PRIV_REQUIRES (мета-компонент `driver` в IDF 6 больше не прокидывает инклуды) |
| 4 | esp_video isp | `case CAM_CTLR_COLOR_YUV422` → 4 варианта (YUYV/UYVY/YVYU/VYUY); `COLOR_SPACE_TYPE()==COLOR_SPACE_RAW` → локальный макрос `COLOR_SPACE_TYPE_IS_RAW()` (удалены в IDF 6) |
| 5 | esp_video csi/dvp/spi | `CAM_CTLR_COLOR_YUV422` → `CAM_CTLR_COLOR_YUV422_YUYV` |
| 6 | esp_lcd_jd9365 | `panel_dev_config->color_space` → `rgb_ele_order` |
| 7 | esp_video csi (runtime!) | на IDF 6 вход CSI = выход (RGB565): без этого `esp_cam_new_csi_ctlr` возвращает NOT_SUPPORTED на чипах < v3.0 (конверсия в CSI bridge запрещена), как в esp_video 2.x |
| 8 | esp_cam_sensor (CMakeLists) | + `esp_driver_spi` в requires (`esp_cam_ctlr_spi.h` инклудит `driver/spi_slave.h`) |
| 9 | app_lcd.c | `pixel_format` → `in_color_format`/`out_color_format` (`LCD_COLOR_FMT_RGB565`); `.flags.use_dma2d` → `esp_lcd_dpi_panel_enable_dma2d()` под `#if IDF>=6` |

Правки 1–7 выполнены в копиях компонентов внутри `components/` (раньше
применялись скриптом к `managed_components/` после каждого `set-target` —
это и было источником ошибок, если шаг пропускали или выполняли в другом
порядке). Правки 8–9 — в обычных файлах репозитория.

### Рантайм-фикс CSI (чипы ревизии < v3.0, включая v1.3 платы JC1060P470C)

Симптом: прошивка собирается и грузится, сенсор детектируется, но при старте
потока:

```
E CSI: esp_cam_new_csi_ctlr(227): failed to configure format conversion
csi_video: video->ops->start=106 (ESP_ERR_NOT_SUPPORTED)
```

Причина: в IDF 6.0 драйвер CSI безусловно вызывает настройку аппаратной
конверсии цвета в CSI bridge, а на ESP32-P4 ревизий ниже v3.0 она запрещена
(проверка `ESP_CHIP_REV_ABOVE(chip_version, 300)`). В IDF 5.5.5 этой проверки
не было вовсе — конверсию RAW10→RGB565 всегда выполнял ISP, а цвета в
конфиге CSI использовались только для расчёта размеров буферов.

Решение (фикс №7 в таблице, как в официальном esp_video 2.x): на IDF 6
входной цвет CSI объявляется равным выходному (RGB565) — DMA и так принимает
выход ISP, а драйвер CSI выбирает режим bypass вместо запрещённой конверсии.
Конвейер не меняется: сенсор RAW10 → ISP (demosaic + конверсия в RGB565) →
CSI DMA → буфер → дисплей.

## Результат верификации (IDF 6.0.3, esp32p4)

- чистая сборка: `set-target` → `build` — exit 0, `video_lcd_display.bin`
  499 184 байт (76 % раздела factory свободно), bootloader 24 272 байта;
- обновление поверх старого клона со «старым» `managed_components/`:
  `pull` → `set-target` → `build` — exit 0 (устаревшие каталоги игнорируются);
- офсеты прошивки IDF 6: bootloader `0x2000`, partition-table `0x10000`, app `0x20000`.

Размер бинаря на 48 байт меньше, чем в сборках через патч `managed_components/`:
в прошивку встраиваются относительные пути исходников, `components/...`
короче `managed_components/...` — функционально прошивки идентичны.

## ESP-IDF 5.5.5

Используйте исходный рабочий репозиторий:
https://github.com/megavatt05/ESP32P4-JC1060P470C-I_W_Y/tree/example/camera-photo-app

Ветка `idf603-build-fixes` (коммиты `b267b43` → `a0d244f` → `50087da` → `12725dc`)
слита в `main` merge-коммитом `c76f602` и сохранена как архив. Ветки целились в
IDF 6.0.x: патчи в `components/` применены безусловно (большинство из них обратно
совместимы с 5.5, но комбинация целиком на 5.5.5 не проверялась).

## История (почему не скрипты)

1. `tools/patch_esp_ipa_idf6.ps1` (удалён) — заменял только заголовок
   isp_types.h без REQUIRES → `fatal error: driver/isp_types.h: No such file or directory`.
2. `patch_idf6_managed_components.py` (удалён) — полный набор патчей, но
   требовал ручного запуска после каждого `set-target`; при пропуске шага или
   изменении порядка команд сборка падала на первозданных компонентах
   (`fatal error: hal/isp_types.h: No such file or directory`).
3. Текущее решение — компоненты зафиксированы в `components/` (пин версий в
   `dependencies.lock`: `source.type: local`). Скрипты не нужны, порядок
   команд не важен, состояние `managed_components/` не имеет значения.

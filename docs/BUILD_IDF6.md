# Сборка JC1060P470C-camera-photo-app

## Рекомендуемый путь: ESP-IDF **5.5.5**

```powershell
. C:\esp\v5.5.5\esp-idf\export.ps1
cd C:\2\JC1060P470C-camera-photo-app
Remove-Item -Recurse -Force build, managed_components, sdkconfig -ErrorAction SilentlyContinue
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Ожидается: `PID=0x5602`, `width=1288 height=728`, `fps: ~30`.

## ESP-IDF **6.0.3** (проверено, собирается с нуля)

Компоненты из registry (esp_video 1.2, esp_ipa, esp_lcd_jd9365) написаны под IDF 5.x,
поэтому после `set-target` их нужно один раз пропатчить скриптом
`patch_idf6_managed_components.py` (находится в корне репозитория).

Скрипт кроссплатформенный (Windows / Linux / macOS), идемпотентный (повторный
запуск безопасен) и со строгой проверкой: при невозможности применить патч он
сообщает об ошибке и возвращает ненулевой код выхода, а не молчит.

**Windows (ESP-IDF PowerShell или CMD — Git Bash НЕ нужен):**

```powershell
. C:\esp\v6.0.3\esp-idf\export.ps1
cd C:\2\JC1060P470C-camera-photo-app
Remove-Item -Recurse -Force build, managed_components, sdkconfig -ErrorAction SilentlyContinue
idf.py set-target esp32p4
python patch_idf6_managed_components.py
idf.py build
idf.py -p COMx flash monitor
```

**Linux / macOS:**

```bash
. $HOME/esp/esp-idf/export.sh          # ваш IDF 6.0.3
cd JC1060P470C-camera-photo-app
rm -rf build managed_components sdkconfig
idf.py set-target esp32p4
python3 patch_idf6_managed_components.py
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

Полезные режимы:

```bash
python patch_idf6_managed_components.py --check   # только проверить состояние, ничего не менять
```

⚠️ `dependencies.lock` удалять не нужно — он фиксирует проверенные версии
компонентов, патчи под которые расписаны.

> **ВАЖНО:** старый `tools/patch_esp_ipa_idf6.ps1` удалён. Он заменял только
> заголовок `hal/isp_types.h → driver/isp_types.h`, без добавления зависимости
> `esp_driver_isp` — это приводило к ошибке
> `fatal error: driver/isp_types.h: No such file or directory`.
> Если вы получаете эту ошибку — вы использовали старый скрипт: выполните
> `python patch_idf6_managed_components.py` и запустите `idf.py build` снова.

### Что патчит скрипт (managed_components, вне git)

| # | Компонент | Исправление |
|---|-----------|-------------|
| 1 | esp_ipa | `hal/isp_types.h` → `driver/isp_types.h` |
| 2 | esp_ipa | + `REQUIRES esp_driver_isp` в CMakeLists (иначе п.1 не работает) |
| 3 | esp_video | + `esp_driver_gpio`, `esp_driver_i2c` в PRIV_REQUIRES |
| 4 | esp_video isp | `case YUV422` → 4 варианта (YUYV/UYVY/YVYU/VYUY); `COLOR_SPACE_TYPE()==COLOR_SPACE_RAW` → `COLOR_SPACE_TYPE_IS_RAW()` |
| 5 | esp_video csi/dvp/spi | `CAM_CTLR_COLOR_YUV422` → `CAM_CTLR_COLOR_YUV422_YUYV` |
| 6 | esp_lcd_jd9365 | `panel_dev_config->color_space` → `rgb_ele_order` |

### Что исправлено в самом репозитории (ветка idf603-build-fixes)

- `components/espressif__esp_cam_sensor/CMakeLists.txt` — + `esp_driver_spi`
  в requires (`esp_cam_ctlr_spi.h` инклудит `driver/spi_slave.h`, в IDF 6
  мета-компонент `driver` больше не прокидывает инклуды);
- `main/app_lcd.c` — `pixel_format` → `in_color_format`/`out_color_format`
  (`LCD_COLOR_FMT_RGB565`); `.flags.use_dma2d` под `#if ESP_IDF_VERSION_MAJOR < 6`,
  вместо него `esp_lcd_dpi_panel_enable_dma2d()` после `esp_lcd_panel_init()`.

### Результат проверки (IDF 6.0.3, esp32p4)

- `idf.py build` — exit 0; проверены оба сценария:
  - чистая сборка с нуля (set-target → patch → build);
  - «спасение» дерева, наполовину пропатченного старым PS1-скриптом
    (та самая ошибка `driver/isp_types.h`) — patch → build;
- `video_lcd_display.bin` 499 232 байт (76 % раздела factory свободно),
  bootloader 24 272 байта;
- офсеты прошивки IDF 6: bootloader `0x2000`, partition-table `0x10000`, app `0x20000`.

Обязательно: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, 1288x728, PM off.

### Частые ошибки

| Ошибка | Причина | Решение |
|--------|---------|---------|
| `fatal error: driver/isp_types.h: No such file or directory` | применена замена заголовка без зависимости `esp_driver_isp` (старый PS1-скрипт) | `python patch_idf6_managed_components.py` и пересобрать |
| `fatal error: hal/isp_types.h: No such file or directory` | патч не применялся вовсе | выполнить патч-скрипт после `set-target` |
| Скрипт вернул `[ОШИБКА] ... паттерн не найден` | версии компонентов отличаются от зафиксированных в `dependencies.lock` | `git checkout dependencies.lock`, удалить `managed_components`, `set-target`, патч, build |

# Camera Photo App — JC1060P470C_I_W_Y

Живой preview с камеры **OV02C10** на дисплей **JD9165 1024×600**  
плата **GUITION JC1060P470C_I_W / _Y** (ESP32-P4 **v1.3** + ESP32-C6).

Целевой фреймворк: **ESP-IDF 6.0.3** (также собирается на 5.5.x).

| | |
|--|--|
| Камера | OV02C10 (MIPI-CSI), PID `0x5602` |
| Режим | RAW10 **1288×728** @ ~30 fps → ISP → RGB565 |
| Дисплей | JD9165, MIPI-DSI 2-lane, 1024×600 |
| SCCB | SDA=**GPIO7**, SCL=**GPIO8** |
| Подсветка | **GPIO23** (LEDC) |
| LDO MIPI | channel **3**, 2.5 V |

---

## Важно: OV02C10 и версии компонентов

Официальный `esp_cam_sensor` из registry **не содержит** OV02C10.

В репозитории:

```
components/espressif__esp_cam_sensor/   ← v1.2.1 + драйвер OV02C10
```

В `main/idf_component.yml`:

```yaml
esp_video:
  version: "~1.2.0"   # НЕ 2.x (требует cam_sensor 2.6 без OV02C10)
```

Если после `idf.py reconfigure` появится `managed_components/espressif__esp_cam_sensor` без OV02C10 — удалите его.

---

## ESP-IDF 6.0.3 — отличи адаптации

1. **Ревизия чипа** (обязательно):
   ```
   CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
   ```
2. **Partition table offset `0x10000`** + `partitions.csv` (первая запись с `0x11000`) — bootloader в IDF 6 больше.
3. Зависимость **`espressif/usb`** — нужна для USB Serial/JTAG на P4 в IDF 6.
4. `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y` — в IDF 6 warnings по умолчанию как errors; сторонние компоненты (esp_video 1.2) могут иначе не собраться.
5. Режим камеры **1288×728** (по умолчанию), не 1080p — на v1.3 в 720p гарантированный запас (1080p также доступен, см. ниже).

---

## Возможности камеры OV02C10 (ветка `ov02c10-full-capability`)

В этой ветке разоблокирован весь достижимый на ESP32-P4 потенциал OV02C10:

- **1920×1080@30 (полные 2 MP)** — переключается в menuconfig
  (Camera Sensors → OV02C10 → Default format select); предел чипа:
  ISP принимает не более 1920 пикселей по ширине;
- **тестовый шаблон** (4 типа цветных полос) для проверки тракта без оптики —
  Example Configuration → Camera Sensor Test Pattern;
- **управление частотой кадров** 30 → ниже (VBLANK/VTS) с расширением
  максимальной экспозиции — Example Configuration → Camera Frame Rate Override;
- **динамический вывод на LCD**: центрированный кроп 1:1 (по умолчанию)
  или вписывание всего кадра с сохранением пропорций (letterbox);
- **динамическое определение SCCB-адреса**: если модуль камеры отвечает
  на нештатном I2C-адресе (ноутбучный модуль, другая SID-перемычка),
  шина сканируется, и сенсор подхватывается автоматически; в логе видна
  и диагностика (кто есть на шине / отвечает ли кто-нибудь вообще).

Полная карта возможностей (включая честный список недостижимого — 60 fps,
HDR, binning — и объяснение причин):
[docs/OV02C10_CAPABILITIES.md](docs/OV02C10_CAPABILITIES.md).

---

## Сборка (IDF 6.0.3) — без патчей и скриптов

Компоненты, требующие адаптации под IDF 6 (esp_video, esp_ipa, esp_lcd_jd9365),
**уже включены в репозиторий в каталоге `components/`** в пропатченном виде —
как и esp_cam_sensor с OV02C10. Ничего патчить не нужно: просто клонируйте и собирайте.

```bash
git clone https://github.com/megavatt05/JC1060P470C-camera-photo-app.git
cd JC1060P470C-camera-photo-app

. $HOME/esp/esp-idf/export.sh   # ваше окружение IDF 6.0.3 (Windows: export.ps1)

idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

> Если вы обновляете уже существующий клон (`git pull`) — обязательно выполните
> `idf.py set-target esp32p4` после pull (или `rm -rf build managed_components sdkconfig`
> перед сборкой), чтобы проект переключился на компоненты из `components/`.
>
> Для IDF 5.5.5 используйте исходный рабочий репозиторий:
> https://github.com/megavatt05/ESP32P4-JC1060P470C-I_W_Y/tree/example/camera-photo-app
>
> Подробности: [docs/BUILD_IDF6.md](docs/BUILD_IDF6.md); полная история фиксов
> (все 9 изменений с кодом до/после, разбор рантайм-фикса CSI):
> [docs/FIXES_IDF603.md](docs/FIXES_IDF603.md).

Ожидаемый лог:

```
chip revision: v1.3
Min chip rev: v0.0 / Max: v1.99
ov02c10: Detected Camera sensor PID=0x5602
app_video: width=1288 height=728
app_video: Video Stream Start
app_main: fps: ~30
```

---

## Документация

- [docs/OV02C10_CAPABILITIES.md](docs/OV02C10_CAPABILITIES.md) — полная карта возможностей OV02C10: режимы, тестовый шаблон, управление fps, пределы чипа
- [docs/CAMERA_DISPLAY.md](docs/CAMERA_DISPLAY.md) — полное описание пайплайна, пинов, init
- [docs/FIXES_IDF603.md](docs/FIXES_IDF603.md) — полный документ фиксов: история миграции на IDF 6.0.3, разбор рантайм-фикса CSI
- [docs/BUILD_IDF6.md](docs/BUILD_IDF6.md) — процедура сборки на IDF 6 (Linux/Windows)
- [CAMERA_PHOTO_APP.md](CAMERA_PHOTO_APP.md) — краткая шпаргалка

Исходная рабочая ветка (IDF 5.5.x):  
https://github.com/megavatt05/ESP32P4-JC1060P470C-I_W_Y/tree/example/camera-photo-app

---

## Структура

```
.
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv          # IDF 6: offset ≥ 0x11000
├── main/
│   ├── main.c
│   ├── app_lcd.c / .h
│   ├── app_video.c / .h
│   ├── idf_component.yml
│   └── CMakeLists.txt
├── components/                        # локальные компоненты (переопределяют registry)
│   ├── espressif__esp_cam_sensor/     # v1.2.1 + драйвер OV02C10
│   ├── espressif__esp_video/          # v1.2.0, адаптирована под IDF 6
│   ├── espressif__esp_ipa/            # v1.1.0, адаптирована под IDF 6
│   └── espressif__esp_lcd_jd9365/     # v1.0.4, адаптирована под IDF 6
└── docs/
    ├── BUILD_IDF6.md
    ├── CAMERA_DISPLAY.md
    ├── FIXES_IDF603.md
    └── OV02C10_IDF6_PLAN.md
```

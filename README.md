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
5. Режим камеры **1288×728**, не 1080p — иначе `ISP: fifo overflow` на v1.3.

---

## Сборка

```bash
git clone https://github.com/megavatt05/JC1060P470C-camera-photo-app.git
cd JC1060P470C-camera-photo-app

# IDF 6.0.3
. $HOME/esp/esp-idf/export.sh   # путь к вашему IDF 6.0.3

rm -rf build managed_components dependencies.lock sdkconfig
idf.py set-target esp32p4
./patch_idf6_managed_components.sh   # патчит managed_components под IDF 6 (обязательно)
idf.py build
idf.py -p COMx flash monitor
```

> На IDF 5.5.x скрипт не нужен — просто `set-target` + `build`.
> Подробности и Windows-вариант: [docs/BUILD_IDF6.md](docs/BUILD_IDF6.md).

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

- [docs/CAMERA_DISPLAY.md](docs/CAMERA_DISPLAY.md) — полное описание пайплайна, пинов, init
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
├── patch_idf6_managed_components.sh   # патч managed_components под IDF 6 (после set-target)
├── main/
│   ├── main.c
│   ├── app_lcd.c / .h
│   ├── app_video.c / .h
│   ├── idf_component.yml
│   └── CMakeLists.txt
├── components/
│   └── espressif__esp_cam_sensor/   # OV02C10
└── docs/
    └── CAMERA_DISPLAY.md
```

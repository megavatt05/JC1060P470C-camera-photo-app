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

## Важно: папка `components/`

`CMakeLists.txt` содержит:

```cmake
set(EXTRA_COMPONENT_DIRS ./components)
```

В `./components/espressif__esp_cam_sensor/` лежит **локальный** драйвер **OV02C10** (v1.2.1).

Если папки `components/` нет — проект **не соберётся**.  
Полный архив: см. release / или скопируйте из рабочей ветки:

```bash
# из рабочего репо (где уже есть components)
git clone -b example/camera-photo-app --depth 1 \
  https://github.com/megavatt05/ESP32P4-JC1060P470C-I_W_Y.git src
cp -a src/video_lcd_display/components ./
cp -a src/video_lcd_display/main/*.c src/video_lcd_display/main/*.h main/
```

Или распакуйте полный tar из artifacts.

---

## ESP-IDF 6.0.3

1. `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`
2. Partition offset **0x10000**, `partitions.csv` с 0x11000
3. `espressif/usb`
4. `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y`
5. `esp_video: "~1.2.0"` (не 2.x)
6. Камера **1288×728**

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Ожидается: `width=1288 height=728`, `fps: ~30`.

Полная документация: [docs/CAMERA_DISPLAY.md](docs/CAMERA_DISPLAY.md)

Рабочая ветка (IDF 5.5, полный код):
https://github.com/megavatt05/ESP32P4-JC1060P470C-I_W_Y/tree/example/camera-photo-app

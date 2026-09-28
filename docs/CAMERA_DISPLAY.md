# Камера + дисплей на JC1060P470C_I_W_Y

Полное описание пайплайна **OV02C10 (MIPI-CSI) → ISP → RGB565 → PPA → JD9165 (MIPI-DSI)**  
для платы **GUITION JC1060P470C_I_W / _Y** (ESP32-P4 rev **v1.3** + ESP32-C6).

Репозиторий / ветка: `example/camera-photo-app`  
Проект: `video_lcd_display/`

---

## 1. Аппаратная платформа

| Параметр | Значение |
|----------|----------|
| Модуль | ESP32-P4 (dual-core RISC-V) + ESP32-C6 (Wi-Fi 6 / BT) |
| Ревизия чипа P4 | **v1.0 / v1.3 (eco2)** — **не** v3.x |
| Flash | 16 MB NOR |
| PSRAM | 32 MB Hex, **200 MHz** |
| Дисплей | 7″ IPS **1024×600**, драйвер **JD9165**, MIPI-DSI **2-lane** |
| Камера | FPC MIPI-CSI, сенсор **OV02C10** (PID `0x5602`), I²C addr `0x36` |
| Touch | GT911 (I²C, не используется в этом примере) |
| Питание MIPI PHY | LDO **channel 3**, **2.5 V** (общий для DSI и CSI) |

### Критично для v1.3

В IDF 5.5.x **взаимоисключающие** опции ревизии:

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y   # обязательно для v1.3
# CONFIG_ESP32P4_REV_MIN_300 is not set
# CONFIG_ESP32P4_REV_MIN_301 is not set
```

Без `SELECTS_REV_LESS_V3` → Illegal Instruction / Guru Meditation / нестабильный ISP.

Частота CPU на v1.3: **360 MHz** (не 400, если нет `ESP_FORCE_400MHZ_ON_REV_LESS_V3`).

---

## 2. Пины, используемые в этом проекте

### 2.1. Камера (OV02C10, MIPI-CSI)

| Сигнал | GPIO / ресурс | Примечание |
|--------|---------------|------------|
| **SCCB SDA** | **GPIO7** | I²C данные сенсора |
| **SCCB SCL** | **GPIO8** | I²C тактирование |
| SCCB частота | 100 kHz | `CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ` |
| SCCB порт I2C | 0 | |
| Camera RESET | **-1** (не подключён / на плате) | |
| Camera PWDN | **-1** | |
| MIPI-CSI data/clk | выделенные линии SoC | FPC 15-pin class |
| Питание PHY | LDO ch3 2.5 V | то же, что у дисплея |

Адрес сенсора на шине: **0x36**.  
Chip ID (PID): **0x5602** — в логе: `ov02c10: Detected Camera sensor PID=0x5602`.

> GPIO7/8 — **общая** I²C-шина платы: на ней же GT911 (touch) и ES8311 (аудио).  
> В этом примере touch/audio не инициализируются, конфликт не возникает.

### 2.2. Дисплей (JD9165, MIPI-DSI)

| Сигнал | GPIO / ресурс | Примечание |
|--------|---------------|------------|
| **Backlight (PWM)** | **GPIO23** | LEDC channel 0, 5 kHz, 10-bit |
| **LCD RESET** | **GPIO5** (типично на плате) / в коде может быть `EXAMPLE_PIN_NUM_LCD_RST` | Active low |
| MIPI-DSI | 2-lane | через `JD9165_PANEL_BUS_DSI_2CH_CONFIG()` |
| LDO MIPI PHY | **channel 3**, **2500 mV** | `esp_ldo_acquire_channel` **до** init DSI |
| Разрешение DPI | **1024 × 600** | RGB565 |
| DPI clock | **52 MHz** | |

**Тайминг DPI (из `app_lcd.c` для JC1060P470):**

| Параметр | Значение |
|----------|----------|
| h_size / v_size | 1024 / 600 |
| hsync_back_porch | 160 |
| hsync_pulse_width | 20 |
| hsync_front_porch | 160 |
| vsync_back_porch | 23 |
| vsync_pulse_width | 10 |
| vsync_front_porch | 12 |
| pixel_format | RGB565 |
| use_dma2d | true |
| num_fbs | 2 (по `CONFIG_EXAMPLE_CAM_BUF_COUNT`) |

### 2.3. Прочие пины платы (не в этом примере, но важно)

| Функция | GPIO |
|---------|------|
| GT911 RST / INT | 22 / 21 |
| ES8311 BCLK / MCLK / LRCLK / DOUT / DIN | 12 / 13 / 10 / 9 / 48 |
| AMP_EN | 11 |
| SDIO (C6 + TF) D0–D3 / CMD / CLK | 39–42 / 44 / 43 |
| C6 Hosted RST / CMD / CLK / D0–D3 | 54 / 19 / 18 / 14–17 |
| Ethernet MDC / MDIO / Power / CLK | 31 / 52 / 51 / 50 |

**Конфликт SDIO:** Wi-Fi (C6) и TF-карта делят одни линии — одновременно не использовать без unmount.

---

## 3. Программный стек

```
OV02C10 (RAW10 Bayer)
        │  MIPI-CSI (1-lane @ 1288×728)
        ▼
   CSI controller (esp_driver_cam)
        │
        ▼
   Hardware ISP (demosaic, AE, AWB, CCM, gamma…)
        │  RGB565
        ▼
   esp_video  (/dev/video0, V4L2-like API)
        │  DQBUF → кадр в PSRAM
        ▼
   Callback camera_video_frame_operation()
        │  PPA SRM (scale / optional rotate)
        ▼
   LCD framebuffer (DPI, 1024×600 RGB565)
        │
        ▼
   esp_lcd_panel_draw_bitmap()
        │  MIPI-DSI 2-lane
        ▼
   JD9165 → панель 7″
```

| Компонент | Версия / источник |
|-----------|-------------------|
| ESP-IDF | **5.5.x** (проверено 5.5.5) |
| `esp_video` | **~1.2.0** (зафиксировано; 2.x несовместим с cam_sensor 1.2.1) |
| `esp_cam_sensor` | **локальный 1.2.1** с драйвером OV02C10 (`components/espressif__esp_cam_sensor`) |
| `esp_lcd_jd9165` | registry |
| `esp_ipa` | подтягивается для ISP pipeline controller |

---

## 4. Режимы камеры OV02C10

В Kconfig сенсора (`sensors/ov02c10/Kconfig.ov02c10`):

| Kconfig | Разрешение | Lane | Рекомендация |
|---------|------------|------|--------------|
| `CAMERA_OV02C10_MIPI_RAW10_1288x728_30FPS` | **1288×728** | 1 | **По умолчанию в проекте** — стабильно на P4 v1.3 |
| `CAMERA_OV02C10_MIPI_RAW10_1920x1080_30FPS` | 1920×1080 | 1 | На v1.3 → **ISP fifo overflow** + WDT |
| `CAMERA_OV02C10_MIPI_RAW10_1920x1080_2LAN_30FPS` | 1920×1080 | 2 | Пробовать только если FPC/модуль 2-lane |

Рабочий результат на плате пользователя:

```
app_video: width=1288 height=728
fps: ~30.14
camera_buf_len: 1831 KB   # 1288*728*2 RGB565
```

---

## 5. Последовательность инициализации (код)

### 5.1. `app_main()` — общая схема

1. **Подсветка** — LEDC на GPIO23 (`bsp_display_brightness_init`).
2. **`app_lcd_init()`** — дисплей (см. ниже).
3. **PPA client** — `ppa_register_client(PPA_OPERATION_SRM)`.
4. **`app_video_main(NULL)`** — инициализация CSI + auto-detect OV02C10.
5. **`app_video_open("/dev/video0", RGB565)`** — V4L2 open, G_FMT / S_FMT.
6. **Буферы** — `heap_caps_aligned_calloc(..., MALLOC_CAP_SPIRAM)` × N.
7. **`app_video_set_bufs()`** — REQBUFS + QBUF.
8. Подсветка 100%.
9. Регистрация callback `camera_video_frame_operation`.
10. **`app_video_stream_task_start()`** — STREAMON + FreeRTOS task.

### 5.2. Дисплей — `app_lcd_init()`

```
lcd_ldo_power_on()                    // LDO ch3 = 2.5 V
esp_lcd_new_dsi_bus(...)              // JD9165_PANEL_BUS_DSI_2CH_CONFIG
esp_lcd_new_panel_io_dbi(...)         // virtual_channel=0, 8-bit cmd/param
esp_lcd_new_panel_jd9165(...)         // vendor init cmds + DPI config 1024×600
esp_lcd_panel_reset / init / disp_on
```

Vendor-последовательность JD9165 — **не** generic default, а таблица Guition в `app_lcd.c` (`#elif CONFIG_BOARD_TYPE_JC1060P470`).

### 5.3. Камера — `app_video_main()` + open

```c
esp_video_init_csi_config_t csi_config = {
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = {
            .port    = 0,
            .scl_pin = 8,   // CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN
            .sda_pin = 7,   // CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN
        },
        .freq = 100000,
    },
    .reset_pin = -1,
    .pwdn_pin  = -1,
};
esp_video_init(&cam_config);   // внутри: probe OV02C10, load format 1288×728
```

Далее POSIX-like API:

| ioctl | Назначение |
|-------|------------|
| `VIDIOC_QUERYCAP` | capability / версия драйвера |
| `VIDIOC_G_FMT` / `S_FMT` | разрешение и pixelformat (RGB565 на выходе ISP) |
| `VIDIOC_REQBUFS` | 2–3 буфера (USERPTR в PSRAM) |
| `VIDIOC_QUERYBUF` / `QBUF` | постановка буферов в очередь |
| `VIDIOC_STREAMON` | старт потока |
| цикл: `DQBUF` → callback → `QBUF` | непрерывный preview |

### 5.4. Передача кадра на экран — callback

В `main.c` → `camera_video_frame_operation()`:

1. Вход: указатель на кадр RGB565 **1288×728** в PSRAM.
2. **PPA Scale-Rotate-Mirror**:
   - `in.block_w/h` = 1024×600 (кроп/область),
   - `out.pic_w/h` = 1024×600,
   - `rotation_angle` = **0°** (для JC1060),
   - `scale_x/y` = 1,
   - color mode RGB565.
3. `ppa_do_scale_rotate_mirror()` → результат в LCD framebuffer.
4. `esp_lcd_panel_draw_bitmap(panel, 0, 0, 1024, 600, lcd_buffer[i])`.

Буферы LCD берутся через `esp_lcd_dpi_panel_get_frame_buffer()` (двойной/тройной буфер DPI).

---

## 6. Конфигурация (`sdkconfig.defaults`)

Ключевые опции проекта:

```
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y

CONFIG_BOARD_TYPE_JC1060P470=y
CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN=8
CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN=7

CONFIG_CAMERA_OV02C10=y
CONFIG_CAMERA_OV02C10_MIPI_RAW10_1288x728_30FPS=y

CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y
CONFIG_PARTITION_TABLE_OFFSET=0x9000
```

`main/idf_component.yml`:

```yaml
esp_video:
  version: "~1.2.0"   # не '*' и не 2.x
```

`main/CMakeLists.txt` — `PRIV_REQUIRES`: `esp_timer`, `esp_pm`, `esp_mm`, `esp_lcd`, `esp_driver_gpio`, `esp_driver_spi`, `esp_driver_ledc`, `esp_driver_ppa`, `esp_driver_isp`, `heap`, `log`.

---

## 7. Сборка и запуск

```bash
cd video_lcd_display
# при смене defaults:
rm -f sdkconfig
rm -rf build managed_components dependencies.lock   # по необходимости

idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Ожидаемый лог (фрагмент):

```
chip revision: v1.3
Min chip rev: v0.0 / Max chip rev: v1.99
jd9165: version: 2.0.2
ov02c10: Detected Camera sensor PID=0x5602
app_video: version: 1.2.0
app_video: width=1288 height=728
app_video: Video Stream Start
W ISP_AWB: Subwindow feature is not supported on REV < 3.0 ...  # нормально
app_main: fps: 30.14
```

---

## 8. Известные особенности и проблемы

| Симптом | Причина / решение |
|---------|-------------------|
| `ISP: fifo overflow` + Interrupt WDT | 1080p на v1.3 слишком тяжёлый → **1288×728** |
| `ESP_CAM_SENSOR_PIXFORMAT_YUV422_YUYV` undeclared | `esp_video` 2.x + cam_sensor 1.2.1 → pin **esp_video ~1.2.0** |
| Illegal Instruction при boot | Не выбран `SELECTS_REV_LESS_V3` |
| Чёрный экран | Нет LDO ch3, backlight GPIO23, неверная vendor-init JD9165 |
| Камера не детектится | SCCB 7/8, FPC, локальный `esp_cam_sensor` с OV02C10 |
| AWB subwindow warning | Только на REV ≥3.0; на v1.3 игнорируется |
| SD + Wi-Fi одновременно | SDIO conflict GPIO39–44 |

---

## 9. Структура файлов проекта

```
video_lcd_display/
├── CMakeLists.txt              # EXTRA_COMPONENT_DIRS=./components
├── sdkconfig.defaults
├── main/
│   ├── main.c                  # app_main, PPA callback, backlight
│   ├── app_lcd.c / .h          # LDO, DSI, JD9165, DPI 1024×600
│   ├── app_video.c / .h        # V4L2 open/stream, buffers
│   ├── Kconfig.projbuild       # пины SCCB, board type, FPS print
│   ├── idf_component.yml       # esp_video ~1.2.0, jd9165, ...
│   └── CMakeLists.txt
├── components/
│   └── espressif__esp_cam_sensor/   # v1.2.1 + sensors/ov02c10/
└── docs/
    └── CAMERA_DISPLAY.md       # этот файл
```

---

## 10. Дальнейшее развитие «фотоаппарата»

1. **Снимок** — JPEG HW encoder (`/dev/video10`) или `esp_jpg` из RGB565 буфера.
2. **Сохранение** — TF-карта (учесть SDIO vs C6) или Ethernet.
3. **Тач** — GT911 на I²C 7/8 (RST22/INT21), кнопка shutter в LVGL.
4. **UI** — LVGL 9 поверх DPI или отдельный canvas.
5. **UVC / RTSP** — примеры `esp_video/examples`.

Официальная документация Espressif:

- https://docs.espressif.com/projects/esp-video-components/en/latest/esp32p4/
- https://components.espressif.com/components/espressif/esp_cam_sensor
- https://components.espressif.com/components/espressif/esp_video

---

*Документ актуален для ветки `example/camera-photo-app`, IDF 5.5.5, плата JC1060P470C_I_W_Y, чип P4 v1.3, сенсор OV02C10 @ 1288×728 @ ~30 fps.*

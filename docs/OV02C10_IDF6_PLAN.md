# План: драйвер OV02C10 для ESP-IDF 6.0.3

**Цель:** рабочий MIPI-CSI preview OV02C10 → JD9165 на JC1060P470C_I_W_Y (ESP32-P4 **v1.3**) под **ESP-IDF 6.0.3**.

**Исходные факты:**
| Параметр | Значение |
|----------|----------|
| PID | `0x5602` |
| SCCB 7-bit | `0x36` |
| Пины SCCB | SDA=GPIO7, SCL=GPIO8 |
| Формат | RAW10 Bayer GBRG |
| Стабильный режим P4 v1.3 | **1288×728 @ ~30 fps**, 1 lane |
| Рабочий стек | esp_video **1.2** + cam_sensor **1.2.1** на **IDF 5.5.5** |

## Фазы

### 0 — Контекс	✅
### 1 — Архитектура IDF 6
**A (preferred):** managed esp_cam_sensor≥2 + esp_video≥2 + esp_ipa≥1.3.1 + local `components/ov02c10/`
**B:** standalone CSI+ISP without esp_video

### 2 — Каркас драйвера ov02c10
detect, set_format, stream, exposure/gain, DETECT_FN

### 3 — Режимы
0: 1288x728 default | 1: 1080p 1lane | 2: 1080p 2lane

### 4 — Статистика
ov02c10_stats_* (detect/stream/sccb/exp/fps)

### 5 — Интеграция в проект
sdkconfig SELECTS_REV_LESS_V3, build on 6.0.3, fps≥25

### 6 — Skill ov02c10-idf6

## Риски
API 2.x ≠ 1.2; IPA JSON; ISP fifo @1080p; pmu/efuse → PM off

# Camera Photo App — JC1060P470C_I_W_Y

Живой preview: **OV02C10** → ISP → **JD9165 1024×600**.

**IDF:** 6.0.3 (адаптировано) / 5.5.x  
**Полная документация:** [docs/CAMERA_DISPLAY.md](docs/CAMERA_DISPLAY.md)

| Параметр | Значение |
|----------|----------|
| Chip | P4 **v1.3** → `SELECTS_REV_LESS_V3` |
| Камера | OV02C10, **1288×728** @ 30 fps |
| SCCB | GPIO7 / GPIO8 |
| Backlight | GPIO23 |
| Partition offset | **0x10000** (`partitions.csv`) |
| esp_video | **~1.2.0** + локальный cam_sensor 1.2.1 |

```bash
idf.py set-target esp32p4 && idf.py build flash monitor
```

# camera-photo-app — JC1060P470C_I_W_Y

Live camera preview (OV02C10 MIPI-CSI) on 7″ JD9165 1024×600 display.

## Board
- GUITION JC1060P470C_I_W_Y (ESP32-P4 + C6)
- Camera: OV02C10
- Display: JD9165 MIPI-DSI 1024×600
- SCCB: SDA=GPIO7, SCL=GPIO8
- Backlight: GPIO23

## Build (ESP-IDF ≥ 5.4, recommend 5.5.x)

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p PORT flash monitor
```

`sdkconfig.defaults` already selects JC1060P470 + OV02C10.

Local component `components/espressif__esp_cam_sensor` includes OV02C10 support (required).

See `CAMERA_PHOTO_APP.md` for details.

# Сборка JC1060P470C-camera-photo-app

## Рекомендуемый путь: ESP-IDF **5.5.5**

```powershell
. C:\esp\v5.5.5\esp-idf\export.ps1
cd C:\2\JC1060P470C-camera-photo-app
Remove-Item -Recurse -Force build, managed_components, dependencies.lock, sdkconfig -ErrorAction SilentlyContinue
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Ожидается: `PID=0x5602`, `width=1288 height=728`, `fps: ~30`.

## ESP-IDF **6.0.3** (экспериментально)

```powershell
. C:\esp\v6.0.3\esp-idf\export.ps1
cd C:\2\JC1060P470C-camera-photo-app
Remove-Item -Recurse -Force build, managed_components, dependencies.lock, sdkconfig -ErrorAction SilentlyContinue
idf.py set-target esp32p4
idf.py reconfigure
powershell -File .\tools\patch_esp_ipa_idf6.ps1   # если hal/isp_types.h
idf.py build
```

Обязательно: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, 1288x728, PM off.

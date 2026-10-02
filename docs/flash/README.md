# Flash Kit — прошивка ESP32-P4 (cambrowser 2f3d1f8)

Готовый комплект прошивки для JC1060P470C. Полная инструкция:
**[FLASH_GUIDE_RU.md](FLASH_GUIDE_RU.md)** (подключение, 4 способа прошивки,
альтернативные варианты, troubleshooting).

## Самый быстрый способ

1. Откройте **[web_flasher_cambrowser.html](https://megavatt05.github.io/JC1060P470C-camera-photo-app/flash/web_flasher_cambrowser.html)**
   в Chrome/Edge (прямая ссылка GitHub Pages — прошивка уже зашита в страницу).
2. «ПОДКЛЮЧИТЬ И ПРОШИТЬ» → COM10 → 1–3 минуты.

Либо локально: скачайте этот файл и откройте двойным кликом — работает без интернета.

## Файлы

| Файл | Что это |
|---|---|
| `web_flasher_cambrowser.html` | Веб-форма прошивки через Web Serial (прошивка встроена) |
| `cambrowser_p4_2f3d1f8_full.bin` | Единый образ для Launchpad/Flash Download Tool (адрес 0x0) |
| `bins/bootloader/bootloader.bin` | Загрузчик (адрес 0x2000) |
| `bins/partition_table/partition-table.bin` | Таблица разделов (адрес 0x10000) |
| `bins/cambrowser.bin` | Приложение (адрес 0x20000) |
| `flash_COM10.bat` | Автопрошивка на COM10 (esptool, ставится сам) |
| `FLASH_GUIDE_RU.md` | Полная инструкция + альтернативные способы подключения |

Скачать одним архивом: [Release flash-kit-v2](https://github.com/megavatt05/JC1060P470C-camera-photo-app/releases/tag/flash-kit-v2) (zip, единый образ и форма — файлами).

## Собрать комплект заново

`scripts/build_web_flasher.py` (в песочнице ассистента) пересобирает HTML-форму
после обновления бинарников; источники бинарников — CI-артефакт `esp32p4-binaries`.

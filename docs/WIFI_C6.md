# Wi-Fi на JC1060P470C (ESP32-P4 + ESP32-C6)

**Ветка:** `feature/wifi-c6`

## Как устроено

- У P4 **нет** своего Wi-Fi.
- На плате **ESP32-C6** — Wi-Fi 6 (только **2.4 ГГц**) по **SDIO**.
- Стек: `esp_hosted` + `esp_wifi_remote` → обычный `esp_wifi_*` API на хосте.

Типичные GPIO C6:

| Сигнал | GPIO |
|--------|------|
| CLK | 18 |
| CMD | 19 |
| D0–D3 | 14–17 |
| RESET | 54 |

## Включение

1. `idf.py menuconfig`
   - **CamBrowser** → **Enable Wi-Fi via ESP32-C6** = y
   - SSID / password
   - **Component config → Wi-Fi Remote → slave target** = **esp32c6**
2. Ethernet можно оставить включённым.
3. `idf.py fullclean build flash monitor`

```
CONFIG_EB_WIFI_ENABLE=y
CONFIG_EB_WIFI_SSID="MyAP"
CONFIG_EB_WIFI_PASSWORD="secret"
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
```

## SD-карта

Wi-Fi (C6) и microSD могут конфликтовать по SDIO.

- **Ethernet + SD** — надёжно для фильмов с карты
- **Wi-Fi** — без кабеля; SD проверять отдельно

## Лог

```
I app_wifi: STA start, SSID="..."
I app_wifi: Got IP: 192.168.x.x
```

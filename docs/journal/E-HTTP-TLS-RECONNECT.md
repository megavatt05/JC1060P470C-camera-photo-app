# E-HTTP-TLS-RECONNECT — обрывы TLS при стриме с archive.org

**Дата:** 2026-10-03  
**Ветка:** `feature/ethernet-browser`  
**Файл:** `main/media/media_player.c`

## Симптом

```
E esp-tls-mbedtls: read error :-0x004C
E transport_base: esp_tls_conn_read error, errno=Socket is not connected
W HTTP_CLIENT: esp_transport_read returned:-76 and errno:128
I ESP_GMF_HTTP: HTTP Open, URI = https://dn....archive.org/...mp4
I ESP_GMF_HTTP: The total size is ...
```

Повторяется каждые ~6–10 с. Плеер при этом часто доходит до PLAYING (512×288).

## Причина

CDN archive.org закрывает длинные TLS-соединения (EOF). Это не баг декодера H.264.
GMF переоткрывает HTTP и продолжает Range-чтение — отсюда «пила» в логе.

## Исправление (смягчение)

В `esp_player_set_buffer_config`:
- extractor_pool_size: 2 MB → **4 MB**
- http_read_buf_size: 256 KB → **512 KB**
- prebuffer_resume_ms: 1500 → **2000**
- rebuffer_resume_ms: 1200 → **1500**

Лог `ESP_GMF_HTTP` → WARN (меньше шума от реконнектов).

Полный обход: воспроизведение с SD (`tools/convert_movie.*`).

## Связано

- E-TWDT-PANIC — `#ifdef CONFIG_ESP_TASK_WDT_PANIC` (IDF 6)

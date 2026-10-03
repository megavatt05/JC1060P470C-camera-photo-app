# E-TWDT-PANIC — CONFIG_ESP_TASK_WDT_PANIC undeclared (IDF 6.0.3)

**Дата:** 2026-10-03  
**Ветка:** `feature/ethernet-browser`  
**Файл:** `main/media/media_player.c`  
**Симптом (сборка):**

```
media_player.c:599:31: error: 'CONFIG_ESP_TASK_WDT_PANIC' undeclared
  .trigger_panic  = CONFIG_ESP_TASK_WDT_PANIC,
```

**Причина:**  
В ESP-IDF 6.x булевы Kconfig-опции при выключенном состоянии (`# CONFIG_ESP_TASK_WDT_PANIC is not set`) **не определяют** макрос в `sdkconfig.h`. Использование `CONFIG_ESP_TASK_WDT_PANIC` как C-константы даёт undeclared.

**Исправление:**  
Значение `trigger_panic` задаётся через `#ifdef CONFIG_ESP_TASK_WDT_PANIC` → `true` / `else` → `false`. Таймаут и маска idle-ядер без изменений (только CPU0).

**Комментарии:** на русском в блоке TWDT (`media_player.c`).  
**Проверка:** `idf.py build` без ошибки undeclared; рантайм-лог `TWDT idle watch: CPU0 only: ESP_OK` сохраняется.

**Связанный рантайм (не в этом чанке):**  
`ESP_GMF_BLOCK: Read timeout` / HTTP reconnect при воспроизведении archive.org — отдельный чанк после успешной сборки.

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

**Важно:** не подставлять `CONFIG_ESP_TASK_WDT_INIT` вместо PANIC — макрос есть, но семантика другая (panic всегда true).

**Комментарии:** на русском в блоке TWDT.  
**Проверка:** `idf.py build` без undeclared; лог `TWDT idle watch: CPU0 only: ESP_OK`.

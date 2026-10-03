/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Тест скорости сети перед воспроизведением фильма.
 * Работает поверх активного интерфейса (Ethernet или Wi-Fi).
 */

#ifndef NET_SPEED_TEST_H
#define NET_SPEED_TEST_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Источник интерфейса, через который прошёл тест */
typedef enum {
    SPEED_IF_UNKNOWN = 0,
    SPEED_IF_ETH,
    SPEED_IF_WIFI,
} speed_iface_t;

typedef struct {
    bool     ok;              /* true если скачали достаточно байт */
    float    mbps;            /* Мбит/с (мегабиты, не мегабайты) */
    uint32_t bytes;           /* скачано байт */
    uint32_t ms;              /* длительность мс */
    speed_iface_t iface;
    char     iface_str[12];   /* "eth" / "wifi" / "?" */
    char     note[48];        /* краткий вердикт для UI */
} speed_result_t;

/**
 * @brief Синхронный HTTP-тест: скачать ~256–512 КБ с CDN и посчитать Мбит/с.
 *        Блокирует вызывающую задачу до 15 с. Не вызывать из ISR.
 */
esp_err_t speed_test_run(speed_result_t *out);

/** Последний результат (после speed_test_run); нули если ещё не запускали */
const speed_result_t *speed_test_last(void);

/** История: до SPEED_HIST_N записей, 0 = самая свежая */
#define SPEED_HIST_N  8
size_t speed_test_history_count(void);
const speed_result_t *speed_test_history_get(size_t index);

/** Рекомендация: достаточно ли для стрима профиля smooth (~250 kbps video) */
bool speed_test_ok_for_smooth(const speed_result_t *r);

#ifdef __cplusplus
}
#endif

#endif /* NET_SPEED_TEST_H */

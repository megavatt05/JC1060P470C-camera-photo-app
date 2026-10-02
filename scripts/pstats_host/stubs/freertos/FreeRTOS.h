/*
 * Host-only FreeRTOS stub for pstats.c syntax/type check.
 * Real meaning of nothing; just enough types for -fsyntax-level checking.
 */
#ifndef HOST_PSTATS_FREERTOS_H
#define HOST_PSTATS_FREERTOS_H
#include <stdint.h>
#include <stddef.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1

#ifdef HOST_PSTATS_RUNTIME_STATS
typedef uint64_t configRUN_TIME_COUNTER_TYPE;
#endif

typedef struct {
    unsigned int mux;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m)  ((void)(m))

void vTaskDelay(int ticks);
#endif

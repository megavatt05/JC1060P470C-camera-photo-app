#ifndef HOST_PSTATS_TASK_H
#define HOST_PSTATS_TASK_H
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                                   int stack, void *arg, int prio,
                                   TaskHandle_t *handle, int core);
void vTaskDelay(int ticks);
#endif

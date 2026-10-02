#ifndef HOST_FREERTOS_H
#define HOST_FREERTOS_H
#include <stdint.h>
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
int vTaskDelay(int ticks);
#endif

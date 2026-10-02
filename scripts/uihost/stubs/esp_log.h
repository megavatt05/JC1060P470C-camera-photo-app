#ifndef HOST_ESP_LOG_H
#define HOST_ESP_LOG_H
#include <stdio.h>

#ifdef HOST_LOG_PRINTF_CHECK
/* Variant used by the functional host tests: the dead printf keeps the log
 * tag and its helper functions "used" (so -Wextra stays clean on the host,
 * where nothing logs) and keeps format-string checking alive. */
#define ESP_LOGI(tag, fmt, ...) \
    do { if (0) { printf(fmt, ##__VA_ARGS__); } (void)(tag); } while (0)
#define ESP_LOGW(tag, fmt, ...) \
    do { if (0) { printf(fmt, ##__VA_ARGS__); } (void)(tag); } while (0)
#define ESP_LOGE(tag, fmt, ...) \
    do { if (0) { printf(fmt, ##__VA_ARGS__); } (void)(tag); } while (0)
#define ESP_LOGD(tag, fmt, ...) \
    do { if (0) { printf(fmt, ##__VA_ARGS__); } (void)(tag); } while (0)
#else
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#endif

#endif

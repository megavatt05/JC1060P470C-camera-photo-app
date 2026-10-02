#ifndef HOST_ESP_HEAP_H
#define HOST_ESP_HEAP_H
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0x01
#define MALLOC_CAP_INTERNAL 0x02
#define MALLOC_CAP_8BIT 0x04
#define heap_caps_malloc(s, caps) malloc(s)
#define heap_caps_free(p) free(p)
#endif

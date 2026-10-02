#ifndef HOST_PSTATS_IDF_ADDITIONS_H
#define HOST_PSTATS_IDF_ADDITIONS_H
#include "freertos/FreeRTOS.h"
#ifdef HOST_PSTATS_RUNTIME_STATS
configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimePercentForCore(BaseType_t core);
#endif
#endif

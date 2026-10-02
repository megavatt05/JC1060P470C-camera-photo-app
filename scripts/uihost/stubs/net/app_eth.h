#ifndef HOST_APP_ETH_H
#define HOST_APP_ETH_H
#include <stdbool.h>
#include "esp_err.h"
typedef enum { APP_ETH_STOPPED = 0, APP_ETH_STARTING, APP_ETH_WAIT_IP, APP_ETH_READY, APP_ETH_FAIL } app_eth_state_t;
esp_err_t app_eth_start(void);
bool app_eth_ready(void);
const char *app_eth_ip_str(void);
const char *app_eth_status_str(void);
app_eth_state_t app_eth_state(void);
#endif

#ifndef HOST_PSTATS_LWIP_NETIF_H
#define HOST_PSTATS_LWIP_NETIF_H
#include "lwip/err.h"
#include "lwip/pbuf.h"
struct netif {
    err_t (*input)(struct pbuf *p, struct netif *inp);
};
extern struct netif *netif_default;
#endif

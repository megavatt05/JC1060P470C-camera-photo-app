/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser network bring-up: W5500 SPI Ethernet -> esp_eth -> esp_netif
 * -> DHCP, with a tiny status API for the on-screen status bar.
 *
 * Pin assignments and SPI parameters come from Kconfig (menu
 * "Ethernet Browser (CamBrowser)"); with EB_W5500_INT = -1 the driver
 * works in polling mode (poll_period_ms = 20) and no INT wire is needed.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/spi_master.h"
#include "sdkconfig.h"
#include "net/app_eth.h"

static const char *TAG = "app_eth";

typedef enum {
    ETH_STATE_DOWN = 0,     /* driver not started or cable detached   */
    ETH_STATE_LINK_UP,      /* PHY link detected, waiting for DHCP    */
    ETH_STATE_GOT_IP,       /* DHCP done, network ready               */
} eth_state_t;

static struct {
    spi_device_handle_t     spi;
    esp_eth_handle_t        eth_handle;
    esp_netif_t            *netif;
    volatile eth_state_t    state;
    char                    ip_str[16];
    char                    reason[48];  /* last status line for the UI */
} s_eth;

static void eth_set_state(eth_state_t st, const char *reason)
{
    s_eth.state = st;
    if (reason != NULL) {
        strlcpy(s_eth.reason, reason, sizeof(s_eth.reason));
    }
    ESP_LOGI(TAG, "state=%d (%s)", (int)st, s_eth.reason);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        /* esp_eth_netif_glue starts the DHCP client on this event itself */
        eth_set_state(ETH_STATE_LINK_UP, "link up, dhcp...");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        eth_set_state(ETH_STATE_DOWN, "cable detached");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "eth driver started");
        break;
    case ETHERNET_EVENT_STOP:
        eth_set_state(ETH_STATE_DOWN, "eth stopped");
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip = &evt->ip_info;

    snprintf(s_eth.ip_str, sizeof(s_eth.ip_str), IPSTR, IP2STR(&ip->ip));
    eth_set_state(ETH_STATE_GOT_IP, "got ip");
    ESP_LOGI(TAG, "Got IP: %s, gw " IPSTR, s_eth.ip_str, IP2STR(&ip->gw));
}

esp_err_t app_eth_start(void)
{
    if (s_eth.eth_handle != NULL) {
        return ESP_OK;  /* already started */
    }
    eth_set_state(ETH_STATE_DOWN, "starting...");

    /* Netif + event loop (idempotent for IDF 5.x where the app may not have
     * done this yet; on repeated calls both return ESP_OK/ESP_ERR_INVALID_STATE
     * which we tolerate). */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                     eth_event_handler, NULL);
    err |= esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                      ip_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event handler registration failed");
        return err;
    }

    /* SPI bus for the W5500 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_EB_W5500_MOSI,
        .miso_io_num = CONFIG_EB_W5500_MISO,
        .sclk_io_num = CONFIG_EB_W5500_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };
    err = spi_bus_initialize((spi_host_device_t)CONFIG_EB_W5500_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_EB_W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_EB_W5500_CS,
        .queue_size = 20,
    };
    if (spi_bus_add_device((spi_host_device_t)CONFIG_EB_W5500_SPI_HOST, &dev_cfg, &s_eth.spi) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return ESP_FAIL;
    }

    /* W5500 MAC + internal PHY of the module */
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(s_eth.spi);
    w5500.int_gpio_num = CONFIG_EB_W5500_INT;
#if CONFIG_EB_W5500_INT < 0
    w5500.poll_period_ms = 20;      /* polling instead of the INT line */
#endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = CONFIG_EB_W5500_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "esp_eth_mac_new_w5500/phy failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_config, &s_eth.eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed");
        return ESP_FAIL;
    }

    /* Netif + glue */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth.netif = esp_netif_new(&netif_cfg);
    if (s_eth.netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth.eth_handle);
    if (glue == NULL || esp_netif_attach(s_eth.netif, glue) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed");
        return ESP_FAIL;
    }

    if (esp_eth_start(s_eth.eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "W5500 started on SPI%d (mosi=%d miso=%d sclk=%d cs=%d int=%d rst=%d)",
             CONFIG_EB_W5500_SPI_HOST,
             CONFIG_EB_W5500_MOSI, CONFIG_EB_W5500_MISO, CONFIG_EB_W5500_SCLK,
             CONFIG_EB_W5500_CS, CONFIG_EB_W5500_INT, CONFIG_EB_W5500_RST);
    return ESP_OK;
}

bool app_eth_ready(void)
{
    return s_eth.state == ETH_STATE_GOT_IP;
}

const char *app_eth_ip_str(void)
{
    return s_eth.ip_str;
}

const char *app_eth_status_str(void)
{
    return s_eth.reason;
}

app_eth_state_t app_eth_state(void)
{
    return (app_eth_state_t)s_eth.state;
}

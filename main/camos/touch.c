/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CamBrowser touch input: minimal I2C drivers for the three most common
 * touch controllers on cheap ESP32-P4 boards (GT911 / FT5x06 / CST816),
 * with automatic probing on the configured I2C bus.
 *
 * v2 changes (touch debugging on real hardware):
 *  - Optional hardware reset of the touch controller before probing
 *    (EB_TOUCH_RST_GPIO, active low; on JC1060P470C the GT911 RST is
 *    wired to GPIO5). A chip held in reset never answers on I2C - the
 *    #1 cause of "no touch controller found".
 *  - Optional EB_TOUCH_INT_GPIO driven low during the reset: the GT911
 *    latches its I2C address from INT at reset release (low -> 0x5D,
 *    high -> 0x14), making the address deterministic.
 *  - Relaxed GT9xx family ID check: GT9271/GT928/etc. report product
 *    IDs other than "911" and were wrongly rejected before.
 *  - Full I2C bus scan (i2c_master_probe, 0x08..0x77) logged at INFO
 *    level on every bus where probing failed, so the real address and
 *    bus can be read straight from the boot log.
 *
 * Registers used (public datasheet knowledge):
 *   GT911   0x5D/0x14, 16-bit regs: product id 0x8140..0x8143,
 *           status 0x814E (bit7 ready, bit3 coords valid),
 *           points at 0x8150, 8 bytes per point
 *   FT5x06  0x38, 8-bit regs: touch count 0x02, points from 0x03 (6 bytes),
 *           vendor id 0xA8
 *   CST816  0x15, 8-bit regs: touch count 0x02, points from 0x03 (6 bytes),
 *           chip type 0xB5
 */

#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "app_lcd.h"
#include "sdkconfig.h"
#include "camos/touch.h"

static const char *TAG = "camos_touch";

typedef enum {
    TOUCH_CHIP_NONE = 0,
    TOUCH_CHIP_GT911,
    TOUCH_CHIP_FT5X06,
    TOUCH_CHIP_CST816,
} touch_chip_t;

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    touch_chip_t chip;
    uint8_t addr;
    int max_points;
} s_tp;

/* --- Hardware reset (GT911 needs it to answer at all) --------------------- */

static void touch_hw_reset(void)
{
#if CONFIG_EB_TOUCH_RST_GPIO >= 0
    gpio_config_t rst_io = {
        .pin_bit_mask = 1ULL << CONFIG_EB_TOUCH_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&rst_io) != ESP_OK) {
        ESP_LOGW(TAG, "touch RST GPIO%d config failed", CONFIG_EB_TOUCH_RST_GPIO);
        return;
    }

#if CONFIG_EB_TOUCH_INT_GPIO >= 0
    /* Drive INT before the reset release: GT911 latches its I2C address
     * from the INT level at reset deassert (low -> 0x5D). */
    gpio_config_t int_io = {
        .pin_bit_mask = 1ULL << CONFIG_EB_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&int_io) == ESP_OK) {
        gpio_set_level(CONFIG_EB_TOUCH_INT_GPIO, 0);
    }
#endif

    gpio_set_level(CONFIG_EB_TOUCH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_EB_TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(60));      /* GT911: 50 ms+ before I2C ready */

#if CONFIG_EB_TOUCH_INT_GPIO >= 0
    /* Release INT: back to input, the chip drives it when data is ready */
    gpio_set_direction(CONFIG_EB_TOUCH_INT_GPIO, GPIO_MODE_INPUT);
#endif

    ESP_LOGI(TAG, "touch hw reset done (RST=GPIO%d, INT=GPIO%d)",
             CONFIG_EB_TOUCH_RST_GPIO, CONFIG_EB_TOUCH_INT_GPIO);
#else
    ESP_LOGI(TAG, "touch hw reset disabled (EB_TOUCH_RST_GPIO=-1)");
#endif
}

/* --- I2C bus plumbing ------------------------------------------------------ */

static esp_err_t tp_bus_init(int port, int scl, int sda)
{
    if (s_tp.bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_tp.bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus port %d (SCL=%d SDA=%d) init failed: %s",
                 port, scl, sda, esp_err_to_name(err));
    }
    return err;
}

/* Diagnostic: log every address that ACKs on this bus. Runs only when the
 * candidate probes failed - its output tells exactly where the controller
 * (or anything else) actually sits. */
static void tp_bus_scan(i2c_master_bus_handle_t bus, int port, int scl, int sda)
{
    ESP_LOGI(TAG, "I2C scan port %d (SCL=%d SDA=%d):", port, scl, sda);
    int found = 0;
    for (uint16_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  responder at 0x%02X", (unsigned)a);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  no I2C devices responded on this bus");
    }
}

/* Add an I2C device and read the chip-identifying register(s).
 * Returns ESP_OK only if the controller answered convincingly. */
static esp_err_t tp_probe_one(uint8_t addr, touch_chip_t chip)
{
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };

    if (i2c_master_bus_add_device(s_tp.bus, &dev_cfg, &dev) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    uint8_t id[4] = {0};
    bool id_ok = false;

    switch (chip) {
    case TOUCH_CHIP_GT911: {
        /* 16-bit register address, high byte first on the wire */
        uint8_t reg[2] = { 0x81, 0x40 };    /* 0x8140 product id */
        err = i2c_master_transmit_receive(dev, reg, 2, id, 4, 100);
        /* GT911 answers "911\0"; GT9271/GT928 answer "9271"/"928\0" etc.
         * Accept any GT9xx ("9" + two digits) - v1 wrongly rejected them. */
        id_ok = (err == ESP_OK) &&
                id[0] == '9' &&
                isdigit((unsigned char)id[1]) &&
                isdigit((unsigned char)id[2]);
        if (err == ESP_OK && !id_ok) {
            ESP_LOGW(TAG, "GT9xx probe 0x%02X: answered with id "
                     "'%c%c%c%c' (0x%02X 0x%02X 0x%02X 0x%02X) - not accepted",
                     addr, id[0], id[1], id[2], id[3],
                     id[0], id[1], id[2], id[3]);
        }
        break;
    }
    case TOUCH_CHIP_FT5X06: {
        uint8_t reg = 0xA8;                 /* firmware/vendor id */
        err = i2c_master_transmit_receive(dev, &reg, 1, id, 1, 100);
        id_ok = (err == ESP_OK) && id[0] != 0x00 && id[0] != 0xFF;
        break;
    }
    case TOUCH_CHIP_CST816: {
        uint8_t reg = 0xB5;                 /* chip type */
        err = i2c_master_transmit_receive(dev, &reg, 1, id, 1, 100);
        id_ok = (err == ESP_OK) && id[0] != 0x00 && id[0] != 0xFF;
        break;
    }
    default:
        break;
    }

    if (!id_ok) {
        i2c_master_bus_rm_device(dev);
        return (err == ESP_OK) ? ESP_ERR_NOT_FOUND : err;
    }

    s_tp.dev  = dev;
    s_tp.chip = chip;
    s_tp.addr = addr;
    s_tp.max_points = (chip == TOUCH_CHIP_GT911) ? 5 : 2;
    return ESP_OK;
}

esp_err_t touch_init(void)
{
    if (s_tp.chip != TOUCH_CHIP_NONE) {
        return ESP_OK;
    }

    /* Small settle delay: controllers need time after their own power-on
     * reset before I2C becomes responsive (GT911 especially). */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Hardware reset BEFORE any probing: a GT911 held in reset by its RST
     * line never ACKs, and every probe below would fail misleadingly. */
    touch_hw_reset();

    static const struct {
        uint8_t addr;
        touch_chip_t chip;
    } candidates[] = {
        { 0x5D, TOUCH_CHIP_GT911 },
        { 0x14, TOUCH_CHIP_GT911 },
        { 0x38, TOUCH_CHIP_FT5X06 },
        { 0x15, TOUCH_CHIP_CST816 },
    };

    /* Bus candidates, tried in order. First: the menuconfig pins (defaults
     * are the former camera-SCCB bus - on JC1060P470-family boards the
     * touch controller shares that I2C bus with the sensor). */
    static const struct {
        int port, scl, sda;
    } buses[] = {
        { CONFIG_EB_TOUCH_I2C_PORT, CONFIG_EB_TOUCH_I2C_SCL, CONFIG_EB_TOUCH_I2C_SDA },
        { 0, 8, 7 },
        { 1, 8, 7 },
    };

    for (size_t b = 0; b < sizeof(buses) / sizeof(buses[0]); b++) {
        if (tp_bus_init(buses[b].port, buses[b].scl, buses[b].sda) != ESP_OK) {
            continue;   /* bus unavailable - try the next candidate */
        }

        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            esp_err_t err = tp_probe_one(candidates[i].addr, candidates[i].chip);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "touch controller found: %s at 0x%02X "
                         "(port %d SCL=%d SDA=%d)",
                         s_tp.chip == TOUCH_CHIP_GT911 ? "GT911" :
                         s_tp.chip == TOUCH_CHIP_FT5X06 ? "FT5x06" : "CST816",
                         s_tp.addr, buses[b].port, buses[b].scl, buses[b].sda);
                return ESP_OK;
            }
            ESP_LOGI(TAG, "probe 0x%02X chip %d failed: %s",
                     candidates[i].addr, candidates[i].chip, esp_err_to_name(err));
        }

        /* nothing on this bus - scan it for diagnostics, then move on */
        tp_bus_scan(s_tp.bus, buses[b].port, buses[b].scl, buses[b].sda);
        i2c_del_master_bus(s_tp.bus);
        s_tp.bus = NULL;
    }

    ESP_LOGW(TAG, "no touch controller found (RST=GPIO%d, tried port %d "
             "SCL=%d SDA=%d, then SCL=8 SDA=7 on ports 0 and 1); "
             "browser runs without input - check the I2C scan above",
             CONFIG_EB_TOUCH_RST_GPIO,
             CONFIG_EB_TOUCH_I2C_PORT, CONFIG_EB_TOUCH_I2C_SCL,
             CONFIG_EB_TOUCH_I2C_SDA);
    return ESP_ERR_NOT_FOUND;
}

/* --- Coordinate reading ----------------------------------------------------- */

static int tp_normalize(int v, int max_v)
{
    if (v < 0) {
        v = 0;
    }
    if (v >= max_v) {
        v = max_v - 1;
    }
    return v;
}

/* Read up to 2 points from FT5x06/CST816 (same 6-byte point layout) */
static int tp_read_short_points(touch_point_t *out, int max_out)
{
    uint8_t reg = 0x02;
    uint8_t buf[13];    /* count + 2 points */

    if (i2c_master_transmit_receive(s_tp.dev, &reg, 1, buf, 1, 20) != ESP_OK) {
        return -1;
    }
    int count = buf[0];
    if (count <= 0) {
        return 0;
    }
    if (count > 2) {
        count = 2;
    }

    reg = 0x03;
    if (i2c_master_transmit_receive(s_tp.dev, &reg, 1, buf, count * 6, 20) != ESP_OK) {
        return -1;
    }

    for (int i = 0; i < count && i < max_out; i++) {
        const uint8_t *p = &buf[i * 6];
        int x = ((p[0] & 0x0F) << 8) | p[1];
        int y = ((p[2] & 0x0F) << 8) | p[3];
        out[i] = (touch_point_t){ .x = x, .y = y, .id = 0 };
    }
    return count;
}

static int tp_read_gt911(touch_point_t *out, int max_out)
{
    uint8_t status_reg[2] = { 0x81, 0x4E };   /* 0x814E buffer status */
    uint8_t status = 0;

    if (i2c_master_transmit_receive(s_tp.dev, status_reg, 2, &status, 1, 20) != ESP_OK) {
        return -1;
    }
    if ((status & 0x80) == 0) {
        return 0;                       /* no new data */
    }

    int count = status & 0x0F;
    if (count > 0) {
        uint8_t pts[40];
        int read_n = (count > 5) ? 5 : count;
        uint8_t pts_reg[2] = { 0x81, 0x50 };   /* 0x8150 first point */
        if (i2c_master_transmit_receive(s_tp.dev, pts_reg, 2, pts, read_n * 8, 20) == ESP_OK) {
            for (int i = 0; i < read_n && i < max_out; i++) {
                const uint8_t *p = &pts[i * 8];
                out[i] = (touch_point_t){ .x = p[1] | (p[2] << 8),
                                          .y = p[3] | (p[4] << 8),
                                          .id = p[0] };
            }
            count = read_n;
        } else {
            count = -1;
        }
    }

    /* Clear the buffer status flag: single 3-byte write 0x00 -> reg 0x814E */
    uint8_t clear[3] = { 0x81, 0x4E, 0x00 };
    i2c_master_transmit(s_tp.dev, clear, 3, 20);
    return count;
}

int touch_poll(touch_point_t *out, int max_out)
{
    if (s_tp.chip == TOUCH_CHIP_NONE || out == NULL || max_out <= 0) {
        return 0;
    }

    int n;
    if (s_tp.chip == TOUCH_CHIP_GT911) {
        n = tp_read_gt911(out, max_out);
    } else {
        n = tp_read_short_points(out, max_out);
    }
    if (n < 0) {
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int x = out[i].x;
        int y = out[i].y;
#if CONFIG_EB_TOUCH_SWAP_XY
        int t = x; x = y; y = t;
#endif
        if (x >= EXAMPLE_LCD_H_RES) {
            x = EXAMPLE_LCD_H_RES - 1;
        }
        if (y >= EXAMPLE_LCD_V_RES) {
            y = EXAMPLE_LCD_V_RES - 1;
        }
#if CONFIG_EB_TOUCH_MIRROR_X
        x = EXAMPLE_LCD_H_RES - 1 - x;
#endif
#if CONFIG_EB_TOUCH_MIRROR_Y
        y = EXAMPLE_LCD_V_RES - 1 - y;
#endif
        out[i].x = tp_normalize(x, EXAMPLE_LCD_H_RES);
        out[i].y = tp_normalize(y, EXAMPLE_LCD_V_RES);
    }
    return n;
}

const char *touch_chip_name(void)
{
    switch (s_tp.chip) {
    case TOUCH_CHIP_GT911:   return "GT911";
    case TOUCH_CHIP_FT5X06:  return "FT5x06";
    case TOUCH_CHIP_CST816:  return "CST816";
    default:                 return "none";
    }
}

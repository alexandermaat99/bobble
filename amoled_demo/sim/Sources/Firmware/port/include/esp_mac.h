#pragma once
// The simulator's "hardware address": the same every time (its id reads 02514d00514d, its name "Bobble 514D")
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH, ESP_MAC_BASE } esp_mac_type_t;
static inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    static const uint8_t m[6] = {0x02, 0x51, 0x4d, 0x00, 0x51, 0x4d};
    memcpy(mac, m, 6);
    return ESP_OK;
}

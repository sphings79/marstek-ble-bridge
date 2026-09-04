#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Join the configured network as a station. Returns once the first attempt has been started. */
esp_err_t wifi_start(void);

/** Whether the bridge currently holds an IP address. */
bool wifi_is_connected(void);

/** Signal strength of the bridge's own WiFi link, in dBm. Zero when not associated. */
int8_t wifi_rssi(void);

/** Whether the fallback access point is currently up. */
bool wifi_fallback_ap_active(void);

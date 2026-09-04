#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Join the configured network as a station. Returns once the first attempt has been started. */
esp_err_t wifi_start(void);

/** Whether the bridge currently holds an IP address. */
bool wifi_is_connected(void);

#pragma once

#include "esp_err.h"

/** Where the web app bundle is mounted. */
#define STORAGE_WEB_ROOT "/web"

/**
 * Bring up non-volatile storage and mount the web partition.
 *
 * NVS holds the bridge's own settings (bound device, password hash, WiFi credentials); the web
 * partition holds the app bundle that was flashed alongside the firmware.
 */
esp_err_t storage_init(void);

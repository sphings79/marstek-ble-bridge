#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * The network-configuration side of the web interface.
 *
 * Served by the firmware rather than from the web partition, because it has to work on a board
 * that has just been flashed: reachable over the fallback access point, before any credentials
 * exist, and even if the app bundle never made it onto the flash.
 */
esp_err_t wifi_setup_register_handlers(httpd_handle_t server);

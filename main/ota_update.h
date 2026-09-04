#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Firmware updates over the network, into whichever app slot is not running.
 *
 * The bridge sits wherever the storage sits, which is rarely next to a computer. Without this,
 * every change means fetching a cable and carrying the board to it.
 */
esp_err_t ota_update_register_handlers(httpd_handle_t server);

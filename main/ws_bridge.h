#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * The byte relay: a WebSocket on /api/ws that carries device bytes in both directions, plus JSON
 * control messages for scanning, binding and connecting.
 *
 * Text frames are control, binary frames are device bytes. Inbound binary frames carry one prefix
 * byte selecting the BLE write kind; outbound ones do not, because one frame is exactly one
 * notification and there is only ever the one kind.
 *
 * Brings up the BLE central as well, since the two are only useful together.
 */
esp_err_t ws_bridge_start(httpd_handle_t server);

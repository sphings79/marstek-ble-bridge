#pragma once

#include "esp_err.h"

/** Bring up the HTTP server that serves the web app and the bridge API. */
esp_err_t web_server_start(void);

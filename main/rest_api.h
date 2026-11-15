#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * REST surface for other systems - Home Assistant first of all - next to the WebSocket the web app
 * uses.
 *
 * Callers are machines, not browsers, so none of this uses the session cookie. They present an API
 * token as `Authorization: Bearer <token>`. The token is created and revoked from the web app
 * (which does need a login), and the bridge keeps only its SHA-256: it cannot show the token again
 * and a dump of the flash does not give it away.
 *
 *   GET    /api/token          session  {"enabled":bool}
 *   POST   /api/token          session  creates a new token, returns it once, retires the old one
 *   DELETE /api/token          session  switches the token and with it the REST commands off
 *   POST   /api/peak-shaving   token    {"slot":0,"enabled":true,"power":600}
 *   GET    /api/peak-shaving   token    last values sent per slot
 *   GET    /api/log            session  the newest link events as text (timing of writes, replies, drops)
 *   GET    /api/status         token    (or session) version, uptime, heap, slots, last command - for monitoring
 *   POST   /api/connection     token    {"slot":0,"action":"connect"|"disconnect"}
 *   GET    /api/settings       session  {"holdSeconds":15,"keepConnected":[false,false,false]}
 *   POST   /api/settings       session  {"holdSeconds":n} (5 to 3600) and/or {"keep":{"slot":n,"enabled":b}}
 *
 * Plain HTTP, like the rest of the bridge: use this on a network you trust.
 */
esp_err_t rest_api_register_handlers(httpd_handle_t server);

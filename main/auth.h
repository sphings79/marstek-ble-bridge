#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Password handling for the bridge.
 *
 * The bridge serves plain HTTP, so the password itself never crosses the network. It stores
 * `key = SHA-256(salt || password)` and proves possession by challenge-response: it hands out the
 * salt plus a single-use nonce, and the browser answers `HMAC-SHA-256(key, nonce)`.
 *
 * What that does not cover, stated plainly because the README says the same:
 * - The session cookie afterwards is readable on the wire. Stealing it grants control until it
 *   expires; it does not reveal the password.
 * - Claiming an unclaimed bridge sends the derived key once, in the clear. Avoiding that needs a
 *   PAKE, which is out of proportion here. The claim window below bounds the exposure instead.
 */
esp_err_t auth_init(void);

/** Whether a password has been set. */
bool auth_is_claimed(void);

/** Whether this request carries a valid session cookie. */
bool auth_request_is_authenticated(httpd_req_t *req);

/** Register the auth endpoints under /api/auth on a running server. */
esp_err_t auth_register_handlers(httpd_handle_t server);

/**
 * Reject the request with 401 unless it is authenticated. Returns true when the caller should
 * carry on; when false, a response has already been sent.
 */
bool auth_guard(httpd_req_t *req);

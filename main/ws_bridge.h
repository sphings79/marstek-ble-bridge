#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Version of the control protocol on /api/ws and of the REST surface next to it. The web app
 * refuses a bridge that reports an older one, so this goes up whenever a message changes shape.
 *
 * 2: storages live in slots. Control messages carry a "slot", binary frames lead with the slot
 *    byte (and, towards the bridge, the write kind after it).
 */
#define WS_BRIDGE_PROTOCOL_VERSION 2

/**
 * The byte relay: a WebSocket on /api/ws that carries device bytes in both directions, plus JSON
 * control messages for scanning, binding and connecting.
 *
 * Text frames are control, binary frames are device bytes. Several storages share the socket, each
 * in its own slot. Inbound binary frames are [slot][write kind][bytes]; outbound ones are
 * [slot][bytes], because one frame is exactly one notification and there is only ever the one kind.
 *
 * Brings up the BLE central as well, since the two are only useful together.
 */
esp_err_t ws_bridge_start(httpd_handle_t server);

/**
 * A socket has gone. Called for every closed session, not only for a polite WebSocket close.
 *
 * A browser that navigates away usually just drops the TCP connection, so the close frame the
 * handler waits for never arrives and the client would otherwise stay on the books forever.
 */
void ws_bridge_session_closed(int fd);

/** Fill `out` with a JSON snapshot of the relay and BLE counters. */
void ws_bridge_stats_json(char *out, size_t len);

/** Slot holding this storage address (case-insensitive), or -1. */
int ws_bridge_slot_for_address(const char *address);

/** Whether a storage is bound to the slot. */
bool ws_bridge_slot_is_bound(uint8_t slot);

/**
 * Send one command to the storage in `slot` without a browser being involved.
 *
 * Connects first when nothing holds the storage and waits up to `connect_timeout_ms` for the link,
 * then writes with response. The link is released again by the usual linger once no browser is
 * watching. Blocks the calling task for the duration of the connect.
 *
 * ESP_ERR_NOT_FOUND: nothing bound to the slot. ESP_ERR_TIMEOUT: the storage did not connect in
 * time. Other errors come from the Bluetooth stack.
 */
esp_err_t ws_bridge_command(uint8_t slot, const uint8_t *data, size_t len, uint32_t connect_timeout_ms);

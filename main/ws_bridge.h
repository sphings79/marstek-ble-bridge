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

/**
 * Make sure the storage in `slot` is connected, connecting for it when nothing holds it, and wait
 * up to `connect_timeout_ms`. Without a browser watching, the hold time starts again afterwards.
 * Same errors as ws_bridge_command().
 */
esp_err_t ws_bridge_ensure_connected(uint8_t slot, uint32_t connect_timeout_ms);

/** Browsers currently on the WebSocket. */
int ws_bridge_client_count(void);

/** Name and address bound to a slot; empty strings when the slot is free. */
const char *ws_bridge_slot_name(uint8_t slot);
const char *ws_bridge_slot_address(uint8_t slot);

/**
 * Seconds the storage links are kept once no browser is watching and no command came in. Stored,
 * 5 to 3600; ESP_ERR_INVALID_ARG outside that range.
 */
uint16_t ws_bridge_hold_seconds(void);
esp_err_t ws_bridge_set_hold_seconds(uint16_t seconds);

/**
 * Keep a slot connected for good: the bridge connects it by itself after start and reconnects it
 * after a drop (5, 10, 30, then every 60 s), and the hold time does not release it. Stored per
 * slot. Costs the vendor app its access - a storage talks to one Bluetooth device at a time.
 */
bool ws_bridge_keep(uint8_t slot);
esp_err_t ws_bridge_set_keep(uint8_t slot, bool keep);

/** Seconds until the next automatic attempt for a kept slot, or -1 when none is pending. */
int ws_bridge_keep_next_attempt_seconds(uint8_t slot);

/**
 * Let go of every storage and stop reconnecting kept slots until the next restart. For anything
 * that needs the radio and the memory to itself, such as downloading an update.
 */
void ws_bridge_release_all(void);

/** A slot was disconnected on purpose: do not reconnect it by itself for a while (kept slots only). */
void ws_bridge_note_manual_disconnect(uint8_t slot);

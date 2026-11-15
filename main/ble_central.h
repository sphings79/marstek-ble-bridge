#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BLE_ADDR_STR_LEN 18   /* "aa:bb:cc:dd:ee:ff" */
#define BLE_NAME_MAX 32

/**
 * How many storages the bridge can hold at once. Every connection costs RAM on both the controller
 * and the host side, and NimBLE is configured for exactly this many (BT_NIMBLE_MAX_CONNECTIONS) -
 * change the two together.
 */
#define BLE_MAX_SLOTS 3

/** Stand-in for "no slot", for the few messages that concern the radio rather than one storage. */
#define BLE_SLOT_NONE 0xFF

typedef struct {
    char name[BLE_NAME_MAX];
    char address[BLE_ADDR_STR_LEN];
    /* NimBLE address type. Connecting needs it, and guessing "public" fails outright on a device
       that advertises a random address. */
    uint8_t addr_type;
    int8_t rssi;
} ble_device_t;

/** Stand-in for "the type is not known", so the caller can fall back rather than guess silently. */
#define BLE_ADDR_TYPE_UNKNOWN 0xFF

typedef enum {
    BLE_STATE_IDLE,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
    BLE_STATE_ERROR,
} ble_state_t;

/** One inbound notification of the storage in `slot`, exactly as it sent it. Runs on the NimBLE host task. */
typedef void (*ble_notify_cb_t)(uint8_t slot, const uint8_t *data, size_t len);

/** Connection lifecycle of one slot. `msg` is NULL unless there is something worth saying. */
typedef void (*ble_state_cb_t)(uint8_t slot, ble_state_t state, const char *msg);

/** Scan finished; `devices` is only valid for the duration of the call. */
typedef void (*ble_scan_cb_t)(const ble_device_t *devices, size_t count);

esp_err_t ble_central_init(ble_notify_cb_t on_notify, ble_state_cb_t on_state);

/** Look for storages in range. Results arrive via the callback when the scan ends. */
esp_err_t ble_central_scan(uint32_t seconds, ble_scan_cb_t on_result);

/**
 * Connect the storage in `slot` by address, discover the Marstek service and subscribe to
 * notifications.
 *
 * Pass BLE_ADDR_TYPE_UNKNOWN to have the type looked up in the last scan results.
 *
 * The controller starts one connection at a time. A request that arrives while another slot is
 * still connecting is remembered and started when that one is done, so callers can simply ask for
 * all their slots in a row.
 */
esp_err_t ble_central_connect(uint8_t slot, const char *address, uint8_t addr_type);

/** Address type the last scan saw for this address, or BLE_ADDR_TYPE_UNKNOWN. */
uint8_t ble_central_addr_type_for(const char *address);

void ble_central_disconnect(uint8_t slot);
void ble_central_disconnect_all(void);

bool ble_central_is_connected(uint8_t slot);

/** When the storage in `slot` last sent anything (or, failing that, when the link came up), in esp_timer microseconds. */
int64_t ble_central_last_notify_us(uint8_t slot);

/** A connection attempt is running or waiting its turn, or the link is up but not usable yet. */
bool ble_central_is_busy(uint8_t slot);

/** Name and address of the connected device in `slot`; empty strings when there is none. */
const char *ble_central_device_name(uint8_t slot);
const char *ble_central_device_address(uint8_t slot);
int8_t ble_central_rssi(uint8_t slot);

/** Negotiated ATT MTU. 23 means no exchange happened, which caps notifications at 20 bytes. */
uint16_t ble_central_mtu(uint8_t slot);

/** Counters for the diagnostics endpoint - the alternative is guessing from a distance. */
typedef struct {
    uint32_t notifications;
    uint32_t notify_bytes;
    /** Writes handed to the stack. Says nothing about whether the device accepted them. */
    uint32_t writes_ok;
    uint32_t writes_failed;
    /** Write requests the device actually acknowledged, and the last ATT error it answered with. */
    uint32_t write_acks;
    uint32_t write_rejects;
    uint16_t last_write_error;
    uint16_t mtu;
    uint16_t tx_handle;
    uint16_t rx_handle;
    uint16_t cccd_handle;
    /** GATT properties of the write characteristic - which kinds of write it actually accepts. */
    uint8_t tx_props;
    /** GATT properties of the notify characteristic - which of notify/indicate it actually offers. */
    uint8_t rx_props;
    /** What we wrote to its configuration descriptor: 0x0001 notify, 0x0002 indicate. */
    uint16_t cccd_written;
    bool subscribed;
    /** Second subscription, on the write characteristic - it advertises notify as well. */
    bool subscribed_tx;
    /** Whether the link is encrypted. Some devices stay silent until it is. */
    bool encrypted;
    uint16_t last_enc_status;
    bool connected;
} ble_stats_t;

void ble_central_stats(uint8_t slot, ble_stats_t *out);

/**
 * Write to the TX characteristic of the storage in `slot`. `with_response` picks between an
 * acknowledged write and a fire-and-forget one - regular commands use the former, OTA chunks the
 * latter.
 */
esp_err_t ble_central_write(uint8_t slot, const uint8_t *data, size_t len, bool with_response);

/**
 * Remember the advertised name of the bound device, so the app can tell which model it is talking
 * to without a round trip. Not cosmetic: it selects the dashboard and guards firmware updates
 * against the wrong model.
 */
void ble_central_set_device_name(uint8_t slot, const char *name);

/**
 * A short in-memory event log, so the timing of a misbehaving link can be read afterwards instead
 * of guessed from totals: when a slot connected, what was written, what came back, when it dropped.
 * The newest entries are kept; `a` and `b` mean different things per event (see the dump).
 */
typedef enum {
    BLE_EV_CONNECT_START = 1,
    BLE_EV_LINK_UP,
    BLE_EV_CONNECT_FAILED,
    BLE_EV_READY,
    BLE_EV_DISCONNECT,
    BLE_EV_ENC,
    BLE_EV_MTU,
    BLE_EV_WRITE,
    BLE_EV_NOTIFY,
    BLE_EV_KEEPALIVE,
    BLE_EV_KEEPALIVE_MISS,
    BLE_EV_CYCLE,
} ble_event_t;

void ble_central_log(ble_event_t type, uint8_t slot, uint16_t a, uint16_t b);

/** Call `cb` with one formatted line per logged event, oldest first. */
typedef void (*ble_log_line_cb_t)(const char *line, void *ctx);
void ble_central_log_dump(ble_log_line_cb_t cb, void *ctx);

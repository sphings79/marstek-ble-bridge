#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define BLE_ADDR_STR_LEN 18   /* "aa:bb:cc:dd:ee:ff" */
#define BLE_NAME_MAX 32

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
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
    BLE_STATE_ERROR,
} ble_state_t;

/** One inbound notification, exactly as the storage sent it. Runs on the NimBLE host task. */
typedef void (*ble_notify_cb_t)(const uint8_t *data, size_t len);

/** Connection lifecycle. `msg` is NULL unless there is something worth saying. */
typedef void (*ble_state_cb_t)(ble_state_t state, const char *msg);

/** Scan finished; `devices` is only valid for the duration of the call. */
typedef void (*ble_scan_cb_t)(const ble_device_t *devices, size_t count);

esp_err_t ble_central_init(ble_notify_cb_t on_notify, ble_state_cb_t on_state);

/** Look for storages in range. Results arrive via the callback when the scan ends. */
esp_err_t ble_central_scan(uint32_t seconds, ble_scan_cb_t on_result);

/**
 * Connect to a device by address, discover the Marstek service and subscribe to notifications.
 *
 * Pass BLE_ADDR_TYPE_UNKNOWN to have the type looked up in the last scan results.
 */
esp_err_t ble_central_connect(const char *address, uint8_t addr_type);

/** Address type the last scan saw for this address, or BLE_ADDR_TYPE_UNKNOWN. */
uint8_t ble_central_addr_type_for(const char *address);

void ble_central_disconnect(void);

bool ble_central_is_connected(void);

/** Name and address of the connected device; empty strings when there is none. */
const char *ble_central_device_name(void);
const char *ble_central_device_address(void);
int8_t ble_central_rssi(void);

/**
 * Write to the storage's TX characteristic. `with_response` picks between an acknowledged write
 * and a fire-and-forget one - regular commands use the former, OTA chunks the latter.
 */
esp_err_t ble_central_write(const uint8_t *data, size_t len, bool with_response);

/**
 * Remember the advertised name of the bound device, so the app can tell which model it is talking
 * to without a round trip. Not cosmetic: it selects the dashboard and guards firmware updates
 * against the wrong model.
 */
void ble_central_set_device_name(const char *name);

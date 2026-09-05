#include "ble_central.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "ble";

// The Marstek service and its two characteristics, as used by the storages: 0xFF00 with 0xFF01 to
// write to and 0xFF02 delivering notifications.
#define SVC_UUID 0xFF00
#define TX_UUID  0xFF01
#define RX_UUID  0xFF02

// Only Marstek devices are of interest, and the advertised name is what identifies them.
#define NAME_PREFIX "MST_"

// Standard GAP characteristic carrying the device name. Read after connecting so the bridge does
// not depend on having scanned first - after a reboot it connects straight to the bound address,
// and the name is what selects the dashboard and guards firmware updates against the wrong model.
#define GAP_DEVICE_NAME_UUID 0x2A00

#define MAX_SCAN_RESULTS 16

static ble_notify_cb_t s_on_notify;
static ble_state_cb_t s_on_state;
static ble_scan_cb_t s_on_scan;

static bool s_host_ready = false;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle = 0;
static uint16_t s_rx_handle = 0;
static uint16_t s_rx_cccd_handle = 0;

static char s_device_name[BLE_NAME_MAX];
static char s_device_address[BLE_ADDR_STR_LEN];
static int8_t s_rssi = 0;
static uint16_t s_mtu = 23;
static bool s_subscribed = false;
static uint8_t s_tx_props = 0;
static uint8_t s_rx_props = 0;
static uint16_t s_cccd_written = 0;
static uint16_t s_tx_cccd_handle = 0;
static bool s_subscribed_tx = false;
static bool s_encrypted = false;
static uint16_t s_last_enc_status = 0;
static ble_stats_t s_stats;

static ble_addr_t s_target_addr;
static bool s_have_target = false;
static bool s_connect_pending = false;

static ble_device_t s_scan_results[MAX_SCAN_RESULTS];
static size_t s_scan_count = 0;

static void set_state(ble_state_t state, const char *msg)
{
    if (s_on_state) {
        s_on_state(state, msg);
    }
}

static void addr_to_str(const ble_addr_t *addr, char *out)
{
    const uint8_t *v = addr->val;   /* NimBLE stores the address little-endian */
    snprintf(out, BLE_ADDR_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             v[5], v[4], v[3], v[2], v[1], v[0]);
}

static bool str_to_addr(const char *str, uint8_t type, ble_addr_t *out)
{
    unsigned v[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        out->val[i] = (uint8_t) v[5 - i];
    }
    out->type = type;

    return true;
}

/* ------------------------------------------------------------------ notifications & discovery */

/**
 * Ask the storage for a bigger ATT MTU.
 *
 * NimBLE does not do this on its own, and the default of 23 leaves 20 usable bytes per
 * notification. Browsers negotiate ~185 without being asked, which is why the same commands work
 * over Web Bluetooth and produced silence here: a hundred-byte response has to survive the trip
 * in one or two pieces rather than six.
 */
static int on_mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t mtu, void *arg)
{
    (void) conn_handle;
    (void) arg;

    if (error->status == 0) {
        s_mtu = mtu;
        ESP_LOGI(TAG, "MTU negotiated: %u bytes", (unsigned) mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange refused (%d), staying at %u", error->status, (unsigned) s_mtu);
    }

    return 0;
}

static void announce_connected(void)
{
    ESP_LOGI(TAG, "Connected to %s (%s)", s_device_name, s_device_address);
    set_state(BLE_STATE_CONNECTED, NULL);
}

static int on_name_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void) conn_handle;
    (void) arg;

    if (error->status == 0 && attr && attr->om) {
        const uint16_t len = OS_MBUF_PKTLEN(attr->om);
        const uint16_t copy = len < BLE_NAME_MAX - 1 ? len : BLE_NAME_MAX - 1;
        if (ble_hs_mbuf_to_flat(attr->om, s_device_name, copy, NULL) == 0) {
            s_device_name[copy] = '\0';
            ESP_LOGI(TAG, "Device reports its name as \"%s\"", s_device_name);
        }
        return 0;
    }

    // Nothing read, or the read finished: report the link either way. A device that will not tell
    // us its name still works; the name kept from the scan or the binding stands in.
    if (error->status == BLE_HS_EDONE || error->status != 0) {
        if (s_device_name[0] == '\0') {
            ESP_LOGW(TAG, "Device did not report a name");
        }
        announce_connected();
    }

    return 0;
}

static int on_tx_subscribe_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    (void) conn_handle;
    (void) attr;
    (void) arg;

    s_subscribed_tx = (error->status == 0);
    ESP_LOGI(TAG, "Second subscription on the write characteristic: %s",
             s_subscribed_tx ? "accepted" : "refused");
    return 0;
}

static int on_tx_descriptors(uint16_t conn_handle, const struct ble_gatt_error *error,
                             uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void) chr_val_handle;
    (void) arg;

    if (error->status == 0 && dsc && s_tx_cccd_handle == 0 &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_tx_cccd_handle = dsc->handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE && s_tx_cccd_handle != 0) {
        static const uint8_t enable[2] = { 0x01, 0x00 };
        ble_gattc_write_flat(conn_handle, s_tx_cccd_handle, enable, sizeof(enable),
                             on_tx_subscribe_done, NULL);
    }

    return 0;
}

static int on_subscribe_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void) attr;
    (void) arg;

    if (error->status != 0) {
        ESP_LOGE(TAG, "Subscribing to notifications failed: %d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(BLE_STATE_ERROR, "Could not subscribe to notifications");
        return 0;
    }

    s_subscribed = true;

    // The write characteristic advertises notify as well. Subscribing to both costs one more
    // descriptor write and removes the guess about which one the device actually uses.
    ble_gattc_disc_all_dscs(conn_handle, s_tx_handle, s_rx_handle, on_tx_descriptors, NULL);

    const ble_uuid16_t name_uuid = BLE_UUID16_INIT(GAP_DEVICE_NAME_UUID);
    if (ble_gattc_read_by_uuid(conn_handle, 1, 0xffff, &name_uuid.u, on_name_read, NULL) != 0) {
        announce_connected();
    }

    return 0;
}

static int on_descriptors(uint16_t conn_handle, const struct ble_gatt_error *error,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void) chr_val_handle;
    (void) arg;

    // Keep the FIRST one found. Discovery runs from the RX value handle upwards to the end of the
    // table, so later characteristics have configuration descriptors too - overwriting would leave
    // us enabling notifications on some unrelated characteristic, and nothing would ever arrive.
    if (error->status == 0 && dsc && s_rx_cccd_handle == 0 &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_rx_cccd_handle = dsc->handle;
        ESP_LOGI(TAG, "Notification descriptor at handle %u", (unsigned) dsc->handle);
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        return 0;
    }

    if (s_rx_cccd_handle == 0) {
        ESP_LOGE(TAG, "Notification descriptor not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(BLE_STATE_ERROR, "Notification descriptor not found");
        return 0;
    }

    // Which bit to set depends on what the characteristic actually offers. Notify and indicate are
    // different values in the same descriptor, and a device that only indicates will accept the
    // notify bit without complaint and then never send anything - which looks exactly like a
    // working subscription that produces no data.
    const bool can_notify = (s_rx_props & BLE_GATT_CHR_PROP_NOTIFY) != 0;
    const bool can_indicate = (s_rx_props & BLE_GATT_CHR_PROP_INDICATE) != 0;

    s_cccd_written = can_notify ? 0x0001 : (can_indicate ? 0x0002 : 0x0001);

    ESP_LOGI(TAG, "RX properties 0x%02x (notify=%d, indicate=%d), enabling 0x%04x",
             s_rx_props, can_notify, can_indicate, s_cccd_written);

    const uint8_t enable[2] = { (uint8_t) (s_cccd_written & 0xff), (uint8_t) (s_cccd_written >> 8) };
    ble_gattc_write_flat(conn_handle, s_rx_cccd_handle, enable, sizeof(enable),
                         on_subscribe_done, NULL);
    return 0;
}

static int on_characteristics(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg)
{
    (void) arg;

    if (error->status == 0 && chr) {
        const uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (uuid == TX_UUID) {
            s_tx_handle = chr->val_handle;
            s_tx_props = chr->properties;
        } else if (uuid == RX_UUID) {
            s_rx_handle = chr->val_handle;
            s_rx_props = chr->properties;
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        return 0;
    }

    if (s_tx_handle == 0 || s_rx_handle == 0) {
        ESP_LOGE(TAG, "Device does not expose the expected characteristics");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(BLE_STATE_ERROR, "Not a Marstek storage");
        return 0;
    }

    // Descriptors live between the value handle and the end of the service; asking from the value
    // handle upwards is enough to find this characteristic's own configuration descriptor.
    ble_gattc_disc_all_dscs(conn_handle, s_rx_handle, 0xffff, on_descriptors, NULL);
    return 0;
}

static int on_service(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *service, void *arg)
{
    (void) arg;

    if (error->status == 0 && service) {
        ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                                on_characteristics, NULL);
        return 0;
    }

    if (error->status == BLE_HS_EDONE && s_tx_handle == 0) {
        ESP_LOGE(TAG, "Marstek service not found");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(BLE_STATE_ERROR, "Marstek service not found");
    }

    return 0;
}

/* -------------------------------------------------------------------------------- GAP events */

static void remember_scan_result(const struct ble_gap_disc_desc *desc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
        return;
    }
    if (fields.name == NULL || fields.name_len < strlen(NAME_PREFIX)) {
        return;
    }
    if (strncmp((const char *) fields.name, NAME_PREFIX, strlen(NAME_PREFIX)) != 0) {
        return;
    }

    char address[BLE_ADDR_STR_LEN];
    addr_to_str(&desc->addr, address);

    // Advertisements repeat; keep one entry per device and refresh its signal strength.
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_results[i].address, address) == 0) {
            s_scan_results[i].rssi = desc->rssi;
            return;
        }
    }

    if (s_scan_count >= MAX_SCAN_RESULTS) {
        return;
    }

    ble_device_t *entry = &s_scan_results[s_scan_count++];
    entry->addr_type = desc->addr.type;
    size_t name_len = fields.name_len < BLE_NAME_MAX - 1 ? fields.name_len : BLE_NAME_MAX - 1;
    memcpy(entry->name, fields.name, name_len);
    entry->name[name_len] = '\0';
    strlcpy(entry->address, address, sizeof(entry->address));
    entry->rssi = desc->rssi;

    ESP_LOGI(TAG, "Found %s (%s) at %d dBm", entry->name, entry->address, entry->rssi);
}

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void) arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            remember_scan_result(&event->disc);
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "Scan finished, %u device(s)", (unsigned) s_scan_count);
            if (s_on_scan) {
                s_on_scan(s_scan_results, s_scan_count);
                s_on_scan = NULL;
            }
            // Reporting idle here would tell the app the storage had gone away, when all that
            // finished was a scan running alongside a perfectly good connection.
            set_state(ble_central_is_connected() ? BLE_STATE_CONNECTED : BLE_STATE_IDLE, NULL);
            break;

        case BLE_GAP_EVENT_CONNECT:
            s_connect_pending = false;
            if (event->connect.status != 0) {
                ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                set_state(BLE_STATE_ERROR, "Could not connect to the storage");
                break;
            }

            s_conn_handle = event->connect.conn_handle;
            s_tx_handle = s_rx_handle = s_rx_cccd_handle = 0;
            s_mtu = 23;

            ble_gattc_exchange_mtu(s_conn_handle, on_mtu_exchanged, NULL);

            // Browsers and BlueZ negotiate this transparently when a device asks for it, so a
            // storage that stays silent on an unencrypted link looks perfectly connected from
            // here. Asking costs nothing if the device does not care.
            {
                const int sec = ble_gap_security_initiate(s_conn_handle);
                if (sec != 0 && sec != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "Could not start pairing: %d", sec);
                }
            }

            ESP_LOGI(TAG, "Link up, discovering services");
            {
                const ble_uuid16_t svc = BLE_UUID16_INIT(SVC_UUID);
                ble_gattc_disc_svc_by_uuid(s_conn_handle, &svc.u, on_service, NULL);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT: {
            // A link that never became usable is not the same thing as one that ended. The
            // storage regularly accepts a connection and drops it again within a second - the
            // next attempt then works - and reporting that as a plain disconnect makes pressing
            // connect look like it did nothing at all.
            const bool was_usable = s_subscribed;

            s_connect_pending = false;
            ESP_LOGI(TAG, "Disconnected, reason 0x%x", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_tx_handle = s_rx_handle = s_rx_cccd_handle = s_tx_cccd_handle = 0;
            s_subscribed = s_subscribed_tx = false;
            s_encrypted = false;
            s_device_name[0] = '\0';

            if (was_usable) {
                set_state(BLE_STATE_DISCONNECTED, NULL);
            } else {
                set_state(BLE_STATE_ERROR,
                          "dropped_early|The storage dropped the connection before it was "
                          "usable. It usually accepts the next attempt.");
            }
            break;
        }

        case BLE_GAP_EVENT_ENC_CHANGE:
            s_last_enc_status = event->enc_change.status;
            {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
                    s_encrypted = desc.sec_state.encrypted;
                }
            }
            ESP_LOGI(TAG, "Encryption change: status %d, encrypted=%d",
                     event->enc_change.status, s_encrypted);
            break;

        case BLE_GAP_EVENT_MTU:
            s_mtu = event->mtu.value;
            ESP_LOGI(TAG, "MTU is now %u bytes", (unsigned) s_mtu);
            break;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            // Hand the payload up exactly as it arrived. One notification is one frame; merging or
            // splitting here would break the reassembler in the web app.
            const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[512];
            if (len <= sizeof(buf) &&
                ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), NULL) == 0 &&
                s_on_notify) {
                s_stats.notifications++;
                s_stats.notify_bytes += len;
                ESP_LOGD(TAG, "Notification, %u bytes", (unsigned) len);
                s_on_notify(buf, len);
            } else {
                ESP_LOGW(TAG, "Dropped a %u byte notification", (unsigned) len);
            }
            break;
        }

        default:
            break;
    }

    return 0;
}

/* -------------------------------------------------------------------------------- host plumbing */

static void on_host_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_host_ready = true;
    ESP_LOGI(TAG, "Bluetooth host ready");
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth host reset, reason %d", reason);
    s_host_ready = false;
}

static void host_task(void *param)
{
    (void) param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_central_init(ble_notify_cb_t on_notify, ble_state_cb_t on_state)
{
    s_on_notify = on_notify;
    s_on_state = on_state;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_host_sync;
    ble_hs_cfg.reset_cb = on_host_reset;

    nimble_port_freertos_init(host_task);

    return ESP_OK;
}

/* -------------------------------------------------------------------------------------- API */

esp_err_t ble_central_scan(uint32_t seconds, ble_scan_cb_t on_result)
{
    if (!s_host_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_count = 0;
    s_on_scan = on_result;

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return ESP_FAIL;
    }

    struct ble_gap_disc_params params = {
        // Active scanning asks for the scan response, which is where the storages put their name.
        .passive = 0,
        .filter_duplicates = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
    };

    set_state(BLE_STATE_SCANNING, NULL);

    int rc = ble_gap_disc(own_addr_type, seconds * 1000, &params, on_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan could not be started: %d", rc);
        s_on_scan = NULL;
        set_state(BLE_STATE_ERROR, "Scan could not be started");
        return ESP_FAIL;
    }

    return ESP_OK;
}

uint8_t ble_central_addr_type_for(const char *address)
{
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_results[i].address, address) == 0) {
            return s_scan_results[i].addr_type;
        }
    }
    return BLE_ADDR_TYPE_UNKNOWN;
}

esp_err_t ble_central_connect(const char *address, uint8_t addr_type)
{
    if (!s_host_ready) {
        ESP_LOGW(TAG, "Cannot connect: Bluetooth host not ready yet");
        return ESP_ERR_INVALID_STATE;
    }

    // Pressing connect twice is the normal thing to do when the first press seems to do nothing.
    // A second ble_gap_connect while one is outstanding just fails, so treat the repeat as a
    // no-op rather than turning it into an error the user has to interpret.
    if (s_connect_pending) {
        // Repeating the state rather than returning quietly. Pressing connect twice is the normal
        // thing to do when the first press seems to do nothing, and answering with silence is
        // exactly what makes it seem that way.
        ESP_LOGI(TAG, "Connection attempt already running, repeating the state");
        set_state(BLE_STATE_CONNECTING, NULL);
        return ESP_OK;
    }

    if (ble_central_is_connected()) {
        ESP_LOGI(TAG, "Already connected");
        set_state(BLE_STATE_CONNECTED, NULL);
        return ESP_OK;
    }

    if (addr_type == BLE_ADDR_TYPE_UNKNOWN) {
        addr_type = ble_central_addr_type_for(address);
    }
    if (addr_type == BLE_ADDR_TYPE_UNKNOWN) {
        // Nothing better to go on. Public is the common case, but say so - if the connection
        // fails, this is the first thing to suspect.
        ESP_LOGW(TAG, "No address type known for %s, assuming public", address);
        addr_type = BLE_ADDR_PUBLIC;
    }

    if (!str_to_addr(address, addr_type, &s_target_addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_have_target = true;

    // A scan in progress would block the connection attempt.
    ble_gap_disc_cancel();

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return ESP_FAIL;
    }

    strlcpy(s_device_address, address, sizeof(s_device_address));
    set_state(BLE_STATE_CONNECTING, NULL);

    s_connect_pending = true;

    int rc = ble_gap_connect(own_addr_type, &s_target_addr, 10000, NULL, on_gap_event, NULL);
    if (rc != 0) {
        s_connect_pending = false;
        ESP_LOGE(TAG, "Connection could not be started: %d", rc);

        // The numeric reason travels to the browser too. Without it the only place it exists is a
        // serial console, which is exactly what a bridge in a cellar does not have.
        char msg[80];
        snprintf(msg, sizeof(msg), "Could not start connecting (NimBLE error %d, address type %u)",
                 rc, (unsigned) addr_type);
        set_state(BLE_STATE_ERROR, msg);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ble_central_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool ble_central_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_tx_handle != 0;
}

const char *ble_central_device_name(void)
{
    return s_device_name;
}

const char *ble_central_device_address(void)
{
    return s_have_target ? s_device_address : "";
}

uint16_t ble_central_mtu(void)
{
    return s_mtu;
}

void ble_central_stats(ble_stats_t *out)
{
    *out = s_stats;
    out->mtu = s_mtu;
    out->tx_handle = s_tx_handle;
    out->rx_handle = s_rx_handle;
    out->cccd_handle = s_rx_cccd_handle;
    out->tx_props = s_tx_props;
    out->rx_props = s_rx_props;
    out->cccd_written = s_cccd_written;
    out->subscribed = s_subscribed;
    out->subscribed_tx = s_subscribed_tx;
    out->encrypted = s_encrypted;
    out->last_enc_status = s_last_enc_status;
    out->connected = ble_central_is_connected();
}

int8_t ble_central_rssi(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int8_t rssi;
        if (ble_gap_conn_rssi(s_conn_handle, &rssi) == 0) {
            s_rssi = rssi;
        }
    }
    return s_rssi;
}

/** Tells us what the device did with a write, which the queueing return value does not. */
static int on_write_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void) conn_handle;
    (void) attr;
    (void) arg;

    if (error->status == 0) {
        s_stats.write_acks++;
    } else {
        s_stats.write_rejects++;
        s_stats.last_write_error = error->status;
        ESP_LOGW(TAG, "Device rejected a write: status %d", error->status);
    }

    return 0;
}

esp_err_t ble_central_write(const uint8_t *data, size_t len, bool with_response)
{
    if (!ble_central_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    // Ask for an acknowledged write only if the characteristic actually offers one. Browsers pick
    // this way too - Chrome's writeValue() looks at the properties and silently uses the
    // unacknowledged form when that is all there is. Sending a write request to a characteristic
    // that only accepts write-without-response gets it refused, and without a callback that
    // refusal is invisible: the queueing call still returns success.
    const bool supports_ack = (s_tx_props & BLE_GATT_CHR_PROP_WRITE) != 0;
    const bool supports_no_ack = (s_tx_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
    const bool acknowledged = with_response ? supports_ack : (supports_no_ack ? false : supports_ack);

    int rc = acknowledged
        ? ble_gattc_write_flat(s_conn_handle, s_tx_handle, data, len, on_write_done, NULL)
        : ble_gattc_write_no_rsp_flat(s_conn_handle, s_tx_handle, data, len);

    if (rc != 0) {
        s_stats.writes_failed++;
        ESP_LOGW(TAG, "Write of %u bytes failed: %d", (unsigned) len, rc);
        return ESP_FAIL;
    }

    s_stats.writes_ok++;
    return ESP_OK;
}

/** Remember the name a scan saw, so the app can identify the model without a round trip. */
void ble_central_set_device_name(const char *name)
{
    strlcpy(s_device_name, name, sizeof(s_device_name));
}

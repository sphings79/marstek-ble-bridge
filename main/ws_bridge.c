#include "ws_bridge.h"

#include <string.h>

#include "auth.h"
#include "ble_central.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "wifi.h"

static const char *TAG = "ws";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_BOUND_ADDR "bound_addr"
#define NVS_KEY_BOUND_NAME "bound_name"
#define NVS_KEY_BOUND_TYPE "bound_type"

// Outbound binary frames are BLE notifications, which never exceed the negotiated MTU. The inbound
// direction carries one extra prefix byte.
#define MAX_FRAME 640

/** Write kind, encoded as the first byte of every inbound binary frame. */
#define WRITE_WITHOUT_RESPONSE 0x00
#define WRITE_WITH_RESPONSE 0x01

// More than one browser on the same bridge is an obvious thing to want - a phone in the cellar and
// a desk machine upstairs - and tracking only one meant the second silently stole the first one's
// place, then took the BLE link down with it when it left.
#define MAX_CLIENTS 4

static httpd_handle_t s_server = NULL;
static int s_clients[MAX_CLIENTS] = { -1, -1, -1, -1 };
static SemaphoreHandle_t s_send_lock;

static uint32_t s_sends_ok = 0;
static uint32_t s_sends_failed = 0;
static uint32_t s_frames_in = 0;

static char s_bound_addr[BLE_ADDR_STR_LEN];
static char s_bound_name[BLE_NAME_MAX];
static uint8_t s_bound_type = BLE_ADDR_TYPE_UNKNOWN;

static void load_binding(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_bound_addr);
    if (nvs_get_str(nvs, NVS_KEY_BOUND_ADDR, s_bound_addr, &len) != ESP_OK) {
        s_bound_addr[0] = '\0';
    }

    len = sizeof(s_bound_name);
    if (nvs_get_str(nvs, NVS_KEY_BOUND_NAME, s_bound_name, &len) != ESP_OK) {
        s_bound_name[0] = '\0';
    }

    // Remembered from the scan that bound this device. Without it a reconnect after a reboot would
    // have to guess, and guessing wrong means the connection never starts.
    if (nvs_get_u8(nvs, NVS_KEY_BOUND_TYPE, &s_bound_type) != ESP_OK) {
        s_bound_type = BLE_ADDR_TYPE_UNKNOWN;
    }

    nvs_close(nvs);

    if (s_bound_addr[0]) {
        ESP_LOGI(TAG, "Bound to %s (%s)", s_bound_name, s_bound_addr);
    }
}

static void store_binding(const char *address, const char *name)
{
    strlcpy(s_bound_addr, address, sizeof(s_bound_addr));
    strlcpy(s_bound_name, name ? name : "", sizeof(s_bound_name));
    s_bound_type = ble_central_addr_type_for(address);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    nvs_set_str(nvs, NVS_KEY_BOUND_ADDR, s_bound_addr);
    nvs_set_str(nvs, NVS_KEY_BOUND_NAME, s_bound_name);
    nvs_set_u8(nvs, NVS_KEY_BOUND_TYPE, s_bound_type);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ------------------------------------------------------------------------------ sending out */

static int client_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] >= 0) n++;
    }
    return n;
}

static bool add_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) return true;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] < 0) { s_clients[i] = fd; return true; }
    }
    return false;
}

static void drop_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) s_clients[i] = -1;
    }
}

static esp_err_t send_to(int fd, httpd_ws_type_t type, const uint8_t *payload, size_t len)
{
    httpd_ws_frame_t frame = {
        .type = type,
        .payload = (uint8_t *) payload,
        .len = len,
        .final = true,
    };

    // The BLE host task and the HTTP task both end up here.
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
    xSemaphoreGive(s_send_lock);

    if (err != ESP_OK) {
        s_sends_failed++;
        ESP_LOGW(TAG, "Send to fd %d failed (%s), dropping it", fd, esp_err_to_name(err));
        drop_client(fd);
    } else {
        s_sends_ok++;
    }

    return err;
}

/** Everything the storage says goes to every browser watching. */
static esp_err_t send_frame(httpd_ws_type_t type, const uint8_t *payload, size_t len)
{
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t last = ESP_ERR_INVALID_STATE;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const int fd = s_clients[i];
        if (fd >= 0) {
            last = send_to(fd, type, payload, len);
        }
    }

    return last;
}

static void send_json(cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    if (text) {
        send_frame(HTTPD_WS_TYPE_TEXT, (const uint8_t *) text, strlen(text));
        cJSON_free(text);
    }
    cJSON_Delete(json);
}

static const char *state_name(ble_state_t state)
{
    switch (state) {
        case BLE_STATE_SCANNING:     return "scanning";
        case BLE_STATE_CONNECTING:   return "connecting";
        case BLE_STATE_CONNECTED:    return "connected";
        case BLE_STATE_DISCONNECTED: return "disconnected";
        case BLE_STATE_ERROR:        return "error";
        default:                     return "idle";
    }
}

/**
 * The device name rides on every status message, not just the first.
 *
 * It is what the web app uses to pick the dashboard and to check a firmware file against the
 * connected model. Sending it once would make both depend on the client having caught one
 * particular message.
 */
static void send_status(ble_state_t state, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "status");
    cJSON_AddStringToObject(json, "state", state_name(state));

    const char *name = ble_central_device_name();
    cJSON_AddStringToObject(json, "deviceName", name[0] ? name : s_bound_name);

    const char *addr = ble_central_device_address();
    cJSON_AddStringToObject(json, "address", addr[0] ? addr : s_bound_addr);

    if (state == BLE_STATE_CONNECTED) {
        cJSON_AddNumberToObject(json, "rssi", ble_central_rssi());
        // Visible in the browser on purpose: 23 means the exchange did not happen, which caps
        // every notification at 20 bytes and is the first thing to suspect when a storage that
        // answers over Web Bluetooth stays silent here.
        cJSON_AddNumberToObject(json, "mtu", ble_central_mtu());
    }

    // The bridge's own link matters as much as the Bluetooth one: a relayed frame crosses both,
    // and a weak WiFi side is the harder of the two to notice from the browser.
    const int8_t wifi = wifi_rssi();
    if (wifi != 0) {
        cJSON_AddNumberToObject(json, "wifiRssi", wifi);
    }
    if (msg) {
        cJSON_AddStringToObject(json, "msg", msg);
    }

    send_json(json);
}

static char *hello_json(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "hello");
    cJSON_AddNumberToObject(json, "version", 1);

    if (s_bound_addr[0]) {
        cJSON *bound = cJSON_CreateObject();
        cJSON_AddStringToObject(bound, "name", s_bound_name);
        cJSON_AddStringToObject(bound, "address", s_bound_addr);
        cJSON_AddItemToObject(json, "bound", bound);
    } else {
        cJSON_AddNullToObject(json, "bound");
    }

    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return text;
}

static void send_error(const char *code, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "error");
    cJSON_AddStringToObject(json, "code", code);
    cJSON_AddStringToObject(json, "msg", msg);
    send_json(json);
}

/* ------------------------------------------------------------------------- BLE -> WebSocket */

static void on_ble_notify(const uint8_t *data, size_t len)
{
    send_frame(HTTPD_WS_TYPE_BINARY, data, len);
}

static void on_ble_state(ble_state_t state, const char *msg)
{
    send_status(state, msg);
}

static void on_scan_finished(const ble_device_t *devices, size_t count)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "scanResult");

    cJSON *list = cJSON_AddArrayToObject(json, "devices");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", devices[i].name);
        cJSON_AddStringToObject(item, "address", devices[i].address);
        cJSON_AddNumberToObject(item, "rssi", devices[i].rssi);
        cJSON_AddItemToArray(list, item);
    }

    send_json(json);
}

/* ------------------------------------------------------------------------- WebSocket -> BLE */

static void handle_control(const char *text, size_t len)
{
    cJSON *json = cJSON_ParseWithLength(text, len);
    if (!json) {
        send_error("bad_message", "Not valid JSON");
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "t");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(json);
        send_error("bad_message", "Missing message type");
        return;
    }

    if (strcmp(type->valuestring, "connect") == 0) {
        if (!s_bound_addr[0]) {
            send_error("not_bound", "No storage selected yet");
        } else if (ble_central_connect(s_bound_addr, s_bound_type) != ESP_OK) {
            // The reason already went out as an error status carrying the NimBLE code; this is
            // only the coarse signal that the attempt did not get off the ground.
            send_error("connect_failed", "Could not start connecting");
        }

    } else if (strcmp(type->valuestring, "disconnect") == 0) {
        ble_central_disconnect();

    } else if (strcmp(type->valuestring, "scan") == 0) {
        const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(json, "seconds");
        uint32_t duration = cJSON_IsNumber(seconds) ? (uint32_t) seconds->valuedouble : 5;
        if (duration < 1 || duration > 30) {
            duration = 5;
        }
        if (ble_central_scan(duration, on_scan_finished) != ESP_OK) {
            send_error("scan_failed", "Could not start scanning");
        }

    } else if (strcmp(type->valuestring, "bind") == 0) {
        const cJSON *address = cJSON_GetObjectItemCaseSensitive(json, "address");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
        if (!cJSON_IsString(address)) {
            send_error("bad_message", "bind needs an address");
        } else {
            store_binding(address->valuestring, cJSON_IsString(name) ? name->valuestring : NULL);
            ble_central_set_device_name(s_bound_name);
            ESP_LOGI(TAG, "Bound to %s", s_bound_addr);
            send_status(BLE_STATE_IDLE, NULL);
        }

    } else {
        ESP_LOGW(TAG, "Ignoring unknown message type %s", type->valuestring);
    }

    cJSON_Delete(json);
}

/**
 * Refuse the upgrade unless the request carries a valid session.
 *
 * This has to happen here rather than in the handler: esp_http_server answers the handshake itself
 * and, as its own source says, does not call the uri handler for it. By the time the handler runs
 * the client already holds a 101, and this is the last point where the HTTP request - and with it
 * the session cookie - is still available.
 */
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!auth_request_is_authenticated(req)) {
        ESP_LOGW(TAG, "Refused an unauthenticated WebSocket");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** The upgrade went through: remember the client and tell it what it is talking to. */
static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);

    if (!add_client(fd)) {
        ESP_LOGW(TAG, "Too many clients, refusing fd %d", fd);
        httpd_sess_trigger_close(s_server, fd);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Client connected (fd %d), %d now watching", fd, client_count());

    // Addressed to the newcomer rather than broadcast: the others already know.
    char *hello = hello_json();
    if (hello) {
        send_to(fd, HTTPD_WS_TYPE_TEXT, (const uint8_t *) hello, strlen(hello));
        cJSON_free(hello);
    }
    send_status(ble_central_is_connected() ? BLE_STATE_CONNECTED : BLE_STATE_IDLE, NULL);

    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t frame = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.len > MAX_FRAME) {
        ESP_LOGW(TAG, "Dropping oversized frame (%u bytes)", (unsigned) frame.len);
        return ESP_OK;
    }

    uint8_t buf[MAX_FRAME + 1];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        return err;
    }

    switch (frame.type) {
        case HTTPD_WS_TYPE_TEXT:
            buf[frame.len] = '\0';
            handle_control((const char *) buf, frame.len);
            break;

        case HTTPD_WS_TYPE_BINARY:
            s_frames_in++;
            if (frame.len < 2) {
                break;
            }
            // First byte selects the write kind; the rest goes to the storage untouched.
            if (ble_central_write(buf + 1, frame.len - 1, buf[0] == WRITE_WITH_RESPONSE) != ESP_OK) {
                send_error("write_failed", "Not connected to a storage");
            }
            break;

        case HTTPD_WS_TYPE_CLOSE: {
            const int fd = httpd_req_to_sockfd(req);
            drop_client(fd);
            ESP_LOGI(TAG, "Client on fd %d left, %d still watching", fd, client_count());

            // Only once nobody is left. A battery accepts one BLE connection at a time, so holding
            // it after every browser has gone would lock out everything else, including the vendor
            // app - but dropping it while another tab is still using it was the older mistake.
            if (client_count() == 0) {
                ble_central_disconnect();
            }
            break;
        }

        default:
            break;
    }

    return ESP_OK;
}

/**
 * Every counter that matters, in one place.
 *
 * Debugging this from a browser two floors away meant one guess per firmware upload. Numbers that
 * can simply be read - notifications in, sends out, writes, handles, MTU - turn that into looking.
 */
void ws_bridge_stats_json(char *out, size_t len)
{
    ble_stats_t b;
    ble_central_stats(&b);

    snprintf(out, len,
             "{\"ble\":{"
             "\"connected\":%s,\"encrypted\":%s,\"encStatus\":%u,"
             "\"subscribed\":%s,\"subscribedTx\":%s,\"cccdWritten\":%u,"
             "\"mtu\":%u,\"txHandle\":%u,\"rxHandle\":%u,\"cccdHandle\":%u,"
             "\"txProps\":%u,\"rxProps\":%u,"
             "\"notifications\":%u,\"notifyBytes\":%u,"
             "\"writesOk\":%u,\"writesFailed\":%u,"
             "\"writeAcks\":%u,\"writeRejects\":%u,\"lastWriteError\":%u"
             "},\"ws\":{"
             "\"clients\":%d,\"framesIn\":%u,\"sendsOk\":%u,\"sendsFailed\":%u"
             "}}",
             b.connected ? "true" : "false",
             b.encrypted ? "true" : "false",
             (unsigned) b.last_enc_status,
             b.subscribed ? "true" : "false",
             b.subscribed_tx ? "true" : "false",
             (unsigned) b.cccd_written,
             (unsigned) b.mtu,
             (unsigned) b.tx_handle,
             (unsigned) b.rx_handle,
             (unsigned) b.cccd_handle,
             (unsigned) b.tx_props,
             (unsigned) b.rx_props,
             (unsigned) b.notifications,
             (unsigned) b.notify_bytes,
             (unsigned) b.writes_ok,
             (unsigned) b.writes_failed,
             (unsigned) b.write_acks,
             (unsigned) b.write_rejects,
             (unsigned) b.last_write_error,
             client_count(),
             (unsigned) s_frames_in,
             (unsigned) s_sends_ok,
             (unsigned) s_sends_failed);
}

esp_err_t ws_bridge_start(httpd_handle_t server)
{
    s_server = server;
    s_send_lock = xSemaphoreCreateMutex();
    if (!s_send_lock) {
        return ESP_ERR_NO_MEM;
    }

    load_binding();
    if (s_bound_name[0]) {
        ble_central_set_device_name(s_bound_name);
    }

    esp_err_t err = ble_central_init(on_ble_notify, on_ble_state);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t route = {
        .uri = "/api/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_pre_handshake,
        .ws_post_handshake_cb = ws_post_handshake,
    };

    return httpd_register_uri_handler(server, &route);
}

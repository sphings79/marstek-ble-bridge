#include "ws_bridge.h"

#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "ble_central.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "wifi.h"

static const char *TAG = "ws";

// How long the storage links are kept after the last browser goes away or the last REST command. A
// page reload is a client leaving and coming back within a second, and tearing the Bluetooth links
// down for that is what made every reload land on a dashboard with no data behind it. Adjustable,
// because the other side of the trade is the vendor app, which cannot connect while we hold the
// storage.
#define HOLD_SECONDS_DEFAULT 15
#define HOLD_SECONDS_MIN 5
#define HOLD_SECONDS_MAX 3600
#define NVS_KEY_HOLD "hold_s"

static uint16_t s_hold_seconds = HOLD_SECONDS_DEFAULT;

#define NVS_NAMESPACE "bridge"

// Outbound binary frames are BLE notifications, which never exceed the negotiated MTU. The inbound
// direction carries two extra prefix bytes: the slot and the write kind.
#define MAX_FRAME 640

/** Write kind, the second byte of every inbound binary frame (the first one is the slot). */
#define WRITE_WITHOUT_RESPONSE 0x00
#define WRITE_WITH_RESPONSE 0x01

// More than one browser on the same bridge is an obvious thing to want - a phone in the cellar and
// a desk machine upstairs - and tracking only one meant the second silently stole the first one's
// place, then took the BLE link down with it when it left.
#define MAX_CLIENTS 4

static void cmd_flush(uint8_t slot);

static httpd_handle_t s_server = NULL;
static int s_clients[MAX_CLIENTS] = { -1, -1, -1, -1 };
static SemaphoreHandle_t s_send_lock;

static uint32_t s_sends_ok = 0;
static uint32_t s_sends_failed = 0;
static uint32_t s_frames_in = 0;

/* ------------------------------------------------------------------------------------ bindings */

typedef struct {
    char addr[BLE_ADDR_STR_LEN];
    char name[BLE_NAME_MAX];
    uint8_t type;
} binding_t;

static binding_t s_bound[BLE_MAX_SLOTS];

// Slots that are to stay connected, and the bookkeeping for reconnecting them.
static bool s_keep[BLE_MAX_SLOTS];
static int s_attempts[BLE_MAX_SLOTS];
static int64_t s_next_attempt_us[BLE_MAX_SLOTS];
static bool s_keep_paused = false;
static esp_timer_handle_t s_keep_timer;

// Pause after a slot was disconnected by hand, so that doing so is not undone a second later.
#define KEEP_HAND_PAUSE_S 300
// Wait after a drop before the first reconnect, then the backoff for failed attempts.
#define KEEP_FIRST_RETRY_S 5
static const int s_backoff_s[] = { 5, 10, 30, 60 };

// A link can still stop answering - the writes are accepted and nothing comes back - and a kept
// slot has nobody watching it. So while no browser is on the page, the bridge asks such a slot for
// its state whenever it has been quiet for a while, and cycles the link when three requests in a
// row stay unanswered. With a browser present it does neither: the web app is talking, and a link
// must never be pulled out from under a firmware transfer that is waiting for the storage.
#define KEEPALIVE_S 15
#define KEEPALIVE_REPLY_S 3
#define KEEPALIVE_MISS_LIMIT 3
// A link that has said nothing for this long, while someone is supposed to be using it, is mute.
#define STALE_LINK_S 45
// The state request the web app polls with: magic, length, direction, command 0x03, XOR checksum.
static const uint8_t s_keepalive_packet[] = { 0x73, 0x05, 0x23, 0x03, 0x56 };

static int64_t s_poll_sent_us[BLE_MAX_SLOTS];
static int s_misses[BLE_MAX_SLOTS];

/**
 * NVS key for one slot's field. Slot 0 keeps the key names from when the bridge only knew one
 * storage, so an installation that is updated finds its binding where it left it.
 */
static void binding_key(uint8_t slot, const char *base, char *out, size_t len)
{
    if (slot == 0) {
        snprintf(out, len, "%s", base);
    } else {
        snprintf(out, len, "%s%u", base, (unsigned) slot);
    }
}

static bool is_bound(uint8_t slot)
{
    return slot < BLE_MAX_SLOTS && s_bound[slot].addr[0] != '\0';
}

static void load_bindings(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        binding_t *b = &s_bound[slot];
        char key[16];

        binding_key(slot, "bound_addr", key, sizeof(key));
        size_t len = sizeof(b->addr);
        if (nvs_get_str(nvs, key, b->addr, &len) != ESP_OK) {
            b->addr[0] = '\0';
        }

        binding_key(slot, "bound_name", key, sizeof(key));
        len = sizeof(b->name);
        if (nvs_get_str(nvs, key, b->name, &len) != ESP_OK) {
            b->name[0] = '\0';
        }

        // Remembered from the scan that bound this device. Without it a reconnect after a reboot
        // would have to guess, and guessing wrong means the connection never starts.
        binding_key(slot, "bound_type", key, sizeof(key));
        if (nvs_get_u8(nvs, key, &b->type) != ESP_OK) {
            b->type = BLE_ADDR_TYPE_UNKNOWN;
        }

        binding_key(slot, "keep_conn", key, sizeof(key));
        uint8_t keep = 0;
        s_keep[slot] = nvs_get_u8(nvs, key, &keep) == ESP_OK && keep == 1;

        if (b->addr[0]) {
            ESP_LOGI(TAG, "Slot %u is bound to %s (%s)%s", (unsigned) slot, b->name, b->addr,
                     s_keep[slot] ? ", kept connected" : "");
        }
    }

    nvs_close(nvs);
}

static void load_hold(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    uint16_t value;
    if (nvs_get_u16(nvs, NVS_KEY_HOLD, &value) == ESP_OK && value >= HOLD_SECONDS_MIN &&
        value <= HOLD_SECONDS_MAX) {
        s_hold_seconds = value;
    }
    nvs_close(nvs);
}

uint16_t ws_bridge_hold_seconds(void)
{
    return s_hold_seconds;
}

esp_err_t ws_bridge_set_hold_seconds(uint16_t seconds)
{
    if (seconds < HOLD_SECONDS_MIN || seconds > HOLD_SECONDS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, NVS_KEY_HOLD, seconds);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        s_hold_seconds = seconds;
    }
    return err;
}

static void store_binding(uint8_t slot, const char *address, const char *name)
{
    binding_t *b = &s_bound[slot];
    strlcpy(b->addr, address, sizeof(b->addr));
    strlcpy(b->name, name ? name : "", sizeof(b->name));
    b->type = ble_central_addr_type_for(address);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    char key[16];
    binding_key(slot, "bound_addr", key, sizeof(key));
    nvs_set_str(nvs, key, b->addr);
    binding_key(slot, "bound_name", key, sizeof(key));
    nvs_set_str(nvs, key, b->name);
    binding_key(slot, "bound_type", key, sizeof(key));
    nvs_set_u8(nvs, key, b->type);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void clear_binding(uint8_t slot)
{
    memset(&s_bound[slot], 0, sizeof(s_bound[slot]));
    s_bound[slot].type = BLE_ADDR_TYPE_UNKNOWN;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    char key[16];
    binding_key(slot, "bound_addr", key, sizeof(key));
    nvs_erase_key(nvs, key);
    binding_key(slot, "bound_name", key, sizeof(key));
    nvs_erase_key(nvs, key);
    binding_key(slot, "bound_type", key, sizeof(key));
    nvs_erase_key(nvs, key);
    // A slot that holds nothing has nothing to keep connected.
    binding_key(slot, "keep_conn", key, sizeof(key));
    nvs_erase_key(nvs, key);
    s_keep[slot] = false;
    nvs_commit(nvs);
    nvs_close(nvs);
}

/** Which slot holds this address, or -1. Used to refuse the same storage twice. */
static int slot_holding(const char *address)
{
    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (s_bound[slot].addr[0] && strcasecmp(s_bound[slot].addr, address) == 0) {
            return slot;
        }
    }
    return -1;
}

int ws_bridge_slot_for_address(const char *address)
{
    return address ? slot_holding(address) : -1;
}

bool ws_bridge_slot_is_bound(uint8_t slot)
{
    return is_bound(slot);
}

bool ws_bridge_keep(uint8_t slot)
{
    return slot < BLE_MAX_SLOTS && s_keep[slot];
}

esp_err_t ws_bridge_set_keep(uint8_t slot, bool keep)
{
    if (slot >= BLE_MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (keep && !is_bound(slot)) {
        return ESP_ERR_NOT_FOUND;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    char key[16];
    binding_key(slot, "keep_conn", key, sizeof(key));
    err = keep ? nvs_set_u8(nvs, key, 1) : nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        s_keep[slot] = keep;
        // Start (or stop) right away rather than at the next tick, and with a fresh backoff.
        s_attempts[slot] = 0;
        s_next_attempt_us[slot] = 0;
        ESP_LOGI(TAG, "Slot %u %s", (unsigned) slot, keep ? "is now kept connected" : "is no longer kept connected");
    }
    return err;
}

int ws_bridge_keep_next_attempt_seconds(uint8_t slot)
{
    if (slot >= BLE_MAX_SLOTS || !s_keep[slot] || !is_bound(slot) || s_keep_paused ||
        ble_central_is_connected(slot)) {
        return -1;
    }
    const int64_t wait = s_next_attempt_us[slot] - esp_timer_get_time();
    return wait > 0 ? (int) (wait / 1000000) : 0;
}

static int client_count(void);

/** Watch a kept slot while nobody else does, and cycle it if it stops answering. */
static void keepalive(uint8_t slot, int64_t now)
{
    // Only kept slots, only without a browser, and only once the storage can answer.
    if (!s_keep[slot] || client_count() > 0 || !ble_central_is_ready(slot)) {
        s_poll_sent_us[slot] = 0;
        s_misses[slot] = 0;
        return;
    }

    // A request is out: wait for the answer, then count it.
    if (s_poll_sent_us[slot]) {
        if (ble_central_last_notify_us(slot) >= s_poll_sent_us[slot]) {
            s_poll_sent_us[slot] = 0;
            s_misses[slot] = 0;
            return;
        }
        if (now - s_poll_sent_us[slot] < (int64_t) KEEPALIVE_REPLY_S * 1000000) {
            return;
        }

        s_poll_sent_us[slot] = 0;
        s_misses[slot]++;
        ble_central_log(BLE_EV_KEEPALIVE_MISS, slot, (uint16_t) s_misses[slot], 0);

        if (s_misses[slot] >= KEEPALIVE_MISS_LIMIT) {
            ESP_LOGW(TAG, "Slot %u stopped answering - cycling the link", (unsigned) slot);
            ble_central_log(BLE_EV_CYCLE, slot, 1, 0);
            s_misses[slot] = 0;
            ble_central_disconnect(slot);
        }
        return;
    }

    // Anything the storage said recently is proof enough that it is there.
    if (now - ble_central_last_notify_us(slot) < (int64_t) KEEPALIVE_S * 1000000) {
        s_misses[slot] = 0;
        return;
    }

    if (ble_central_write_quiet(slot, s_keepalive_packet, sizeof(s_keepalive_packet), false) == ESP_OK) {
        s_poll_sent_us[slot] = now;
    }
}

/**
 * Bring kept slots up and back up. Once a second is plenty: the real pacing is the backoff.
 */
static void keep_tick(void *arg)
{
    (void) arg;

    if (s_keep_paused) {
        return;
    }

    const int64_t now = esp_timer_get_time();

    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (ble_central_is_connected(slot)) {
            keepalive(slot, now);
        }
        if (!s_keep[slot] || !is_bound(slot)) {
            continue;
        }
        if (ble_central_is_connected(slot)) {
            continue;
        }
        if (ble_central_is_busy(slot)) {
            continue;
        }
        if (now < s_next_attempt_us[slot]) {
            continue;
        }

        const esp_err_t err = ble_central_connect(slot, s_bound[slot].addr, s_bound[slot].type);
        if (err == ESP_ERR_INVALID_STATE) {
            // Bluetooth is not up yet, which is normal in the first seconds. Not a failed attempt.
            continue;
        }

        const int index = s_attempts[slot] < (int) (sizeof(s_backoff_s) / sizeof(s_backoff_s[0]))
                              ? s_attempts[slot]
                              : (int) (sizeof(s_backoff_s) / sizeof(s_backoff_s[0])) - 1;
        s_next_attempt_us[slot] = now + (int64_t) s_backoff_s[index] * 1000000;
        s_attempts[slot]++;
        ESP_LOGI(TAG, "Slot %u: automatic connect (attempt %d)", (unsigned) slot, s_attempts[slot]);
    }
}

/** Disconnecting a kept slot by hand is not meant to be undone straight away. */
static void hold_off_reconnect(uint8_t slot)
{
    if (slot < BLE_MAX_SLOTS && s_keep[slot]) {
        s_next_attempt_us[slot] = esp_timer_get_time() + (int64_t) KEEP_HAND_PAUSE_S * 1000000;
    }
}

void ws_bridge_note_manual_disconnect(uint8_t slot)
{
    hold_off_reconnect(slot);
}

void ws_bridge_release_all(void)
{
    s_keep_paused = true;
    ble_central_disconnect_all();
}

/* ------------------------------------------------------------------------------ sending out */

static esp_timer_handle_t s_linger_timer;
static esp_timer_handle_t s_refresh_timer;

/**
 * How many times a refreshed link is retried before giving up and leaving it to the browser.
 *
 * The storage regularly accepts the reconnect and drops it again straight away; the attempt after
 * that works. Retrying here is the difference between a reload that just works and one that ends
 * on an error the reader has to act on.
 */
#define REFRESH_RETRIES 3

static int s_refresh_left[BLE_MAX_SLOTS];
// Slots whose link is to be brought up again when the refresh timer fires.
static uint8_t s_refresh_mask;

// Set while no browser is watching. A storage link that was left running with nobody on it stops
// answering - the writes still go out and are still accepted, and nothing comes back - so it is
// refreshed rather than handed over as it is.
static bool s_link_abandoned = false;

// A command that came in over REST waits for the link it asked for; the state callback ends the
// wait. One semaphore per slot, because two slots can be connecting at once.
static SemaphoreHandle_t s_connect_wait[BLE_MAX_SLOTS];
static volatile bool s_waiting[BLE_MAX_SLOTS];

static int client_count(void);

/** Nobody came back, so let the storages go. */
static void linger_expired(void *arg)
{
    (void) arg;

    if (client_count() > 0) {
        return;
    }

    ESP_LOGI(TAG, "Nobody came back - releasing the storages that are not kept connected");
    s_link_abandoned = false;
    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (!s_keep[slot]) {
            ble_central_disconnect(slot);
        }
    }
}

static void cancel_linger(void)
{
    if (s_linger_timer) {
        esp_timer_stop(s_linger_timer);
    }
}

/** Reconnect once the old links have finished going away. */
static void refresh_link(void *arg)
{
    (void) arg;

    if (client_count() == 0) {
        s_refresh_mask = 0;
        return;
    }

    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (!(s_refresh_mask & (1u << slot))) {
            continue;
        }
        s_refresh_mask &= (uint8_t) ~(1u << slot);

        if (is_bound(slot)) {
            ESP_LOGI(TAG, "Re-establishing slot %u for the browser that just arrived", (unsigned) slot);
            ble_central_connect(slot, s_bound[slot].addr, s_bound[slot].type);
        }
    }
}

static void arm_refresh_timer(uint32_t delay_ms)
{
    if (!s_refresh_timer) {
        const esp_timer_create_args_t args = { .callback = &refresh_link, .name = "ws_refresh" };
        if (esp_timer_create(&args, &s_refresh_timer) != ESP_OK) {
            return;
        }
    }

    esp_timer_stop(s_refresh_timer);
    esp_timer_start_once(s_refresh_timer, (uint64_t) delay_ms * 1000);
}

/**
 * Hand a returning browser links that actually talk.
 *
 * Keeping the links across a reload is what stops every refresh from dropping the battery, but a
 * link nobody used in the meantime comes back mute: the storage accepts writes and answers none
 * of them. Cycling it is the only thing found to bring it back.
 */
static void refresh_abandoned_links(void)
{
    const bool abandoned = s_link_abandoned;
    s_link_abandoned = false;

    const int64_t now = esp_timer_get_time();

    bool any = false;
    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (!ble_central_is_connected(slot)) {
            continue;
        }

        // A link that has answered recently is healthy, however long ago the last browser left:
        // that is what the keepalive of a kept slot makes sure of, and cycling it would only
        // show the user a dropped connection for nothing. Anything else that was left without a
        // watcher is cycled, and so is a link that went quiet while a browser supposedly had it -
        // a phone that was closed leaves a half-dead socket behind, so the bridge never noticed
        // that nobody was there.
        const bool fresh = (now - ble_central_last_notify_us(slot)) < (int64_t) STALE_LINK_S * 1000000;
        const bool cycle = s_keep[slot] ? !fresh : (abandoned || !fresh);
        if (!cycle) {
            continue;
        }

        s_refresh_left[slot] = REFRESH_RETRIES;
        s_refresh_mask |= (uint8_t) (1u << slot);
        ble_central_log(BLE_EV_CYCLE, slot, 2, abandoned ? 1 : 0);
        ble_central_disconnect(slot);
        any = true;
    }

    if (any) {
        arm_refresh_timer(1200);
    }
}

static void start_linger(void)
{
    if (!s_linger_timer) {
        const esp_timer_create_args_t args = { .callback = &linger_expired, .name = "ws_linger" };
        if (esp_timer_create(&args, &s_linger_timer) != ESP_OK) {
            // No timer, no grace period - fall back to the old behaviour rather than holding the
            // batteries' connections forever.
            for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
                if (!s_keep[slot]) {
                    ble_central_disconnect(slot);
                }
            }
            return;
        }
    }

    s_link_abandoned = true;
    esp_timer_stop(s_linger_timer);
    esp_timer_start_once(s_linger_timer, (uint64_t) s_hold_seconds * 1000 * 1000);
    ESP_LOGI(TAG, "Nobody watching - holding the storages for %u s in case a browser comes back",
             (unsigned) s_hold_seconds);
}

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

/** Returns whether the socket was on the books. */
static bool drop_client(int fd)
{
    bool found = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            s_clients[i] = -1;
            found = true;
        }
    }
    return found;
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

/** Everything the storages say goes to every browser watching. */
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
static void send_status(uint8_t slot, ble_state_t state, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "status");
    cJSON_AddNumberToObject(json, "slot", slot);
    cJSON_AddStringToObject(json, "state", state_name(state));

    const char *name = ble_central_device_name(slot);
    cJSON_AddStringToObject(json, "deviceName", name[0] ? name : s_bound[slot].name);

    const char *addr = ble_central_device_address(slot);
    cJSON_AddStringToObject(json, "address", addr[0] ? addr : s_bound[slot].addr);

    if (state == BLE_STATE_CONNECTED) {
        cJSON_AddNumberToObject(json, "rssi", ble_central_rssi(slot));
        // Visible in the browser on purpose: 23 means the exchange did not happen, which caps
        // every notification at 20 bytes and is the first thing to suspect when a storage that
        // answers over Web Bluetooth stays silent here.
        cJSON_AddNumberToObject(json, "mtu", ble_central_mtu(slot));
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
    cJSON_AddNumberToObject(json, "version", WS_BRIDGE_PROTOCOL_VERSION);

    cJSON *slots = cJSON_AddArrayToObject(json, "slots");
    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (is_bound(slot)) {
            cJSON *bound = cJSON_CreateObject();
            cJSON_AddStringToObject(bound, "name", s_bound[slot].name);
            cJSON_AddStringToObject(bound, "address", s_bound[slot].addr);
            cJSON_AddItemToArray(slots, bound);
        } else {
            cJSON_AddItemToArray(slots, cJSON_CreateNull());
        }
    }

    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return text;
}

static void send_error(uint8_t slot, const char *code, const char *msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "error");
    if (slot != BLE_SLOT_NONE) {
        cJSON_AddNumberToObject(json, "slot", slot);
    }
    cJSON_AddStringToObject(json, "code", code);
    cJSON_AddStringToObject(json, "msg", msg);
    send_json(json);
}

/* ------------------------------------------------------------------------- BLE -> WebSocket */

// One notification is one frame, with the slot in front. Only the NimBLE host task writes here,
// so a single buffer does; a second copy on that task's small stack would not.
static uint8_t s_notify_frame[1 + 512];

static void on_ble_notify(uint8_t slot, const uint8_t *data, size_t len)
{
    if (len > sizeof(s_notify_frame) - 1) {
        return;
    }

    s_notify_frame[0] = slot;
    memcpy(s_notify_frame + 1, data, len);
    send_frame(HTTPD_WS_TYPE_BINARY, s_notify_frame, len + 1);
}

static void on_ble_state(uint8_t slot, ble_state_t state, const char *msg)
{
    send_status(slot, state, msg);

    if (state == BLE_STATE_DISCONNECTED || state == BLE_STATE_ERROR || state == BLE_STATE_CONNECTED) {
        // Nothing queued for the old link makes sense on a new one.
        cmd_flush(slot);
    }

    if (state == BLE_STATE_CONNECTED) {
        s_attempts[slot] = 0;
        s_misses[slot] = 0;
        s_poll_sent_us[slot] = 0;
    } else if (state == BLE_STATE_DISCONNECTED && s_keep[slot]) {
        // It was up and dropped: try again soon, with a fresh backoff.
        s_attempts[slot] = 0;
        s_next_attempt_us[slot] = esp_timer_get_time() + (int64_t) KEEP_FIRST_RETRY_S * 1000000;
    }

    // Somebody asked over REST and is waiting for the outcome.
    if (s_waiting[slot] && (state == BLE_STATE_CONNECTED || state == BLE_STATE_ERROR)) {
        xSemaphoreGive(s_connect_wait[slot]);
    }

    // A refresh that the storage refused. It usually takes the next one.
    if (state == BLE_STATE_ERROR && s_refresh_left[slot] > 0 && client_count() > 0) {
        s_refresh_left[slot]--;
        ESP_LOGI(TAG, "Slot %u: refresh refused, %d attempt(s) left", (unsigned) slot,
                 s_refresh_left[slot]);
        s_refresh_mask |= (uint8_t) (1u << slot);
        arm_refresh_timer(2500);
    } else if (state == BLE_STATE_CONNECTED) {
        s_refresh_left[slot] = 0;
    }
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

/* ------------------------------------------------------------------------- command queue */

// A storage handles one request at a time and drops one that arrives while it is busy with the
// previous - the event log shows the second of two commands 5 ms apart never being answered. The
// web app paces its writes, but over WiFi two of them can reach the bridge in the same instant, so
// the pacing that counts happens here: per slot, one ordinary command goes out, the next waits for
// its answer (or two seconds) and a short pause. Firmware chunks are written without response and
// bypass this; they have their own flow control.
#define CMD_QUEUE_DEPTH 6
#define CMD_MAX_LEN 160
#define CMD_REPLY_TIMEOUT_US (2LL * 1000 * 1000)
#define CMD_GAP_US (100LL * 1000)

typedef struct {
    uint8_t len;
    uint8_t data[CMD_MAX_LEN];
} queued_cmd_t;

static queued_cmd_t s_cmdq[BLE_MAX_SLOTS][CMD_QUEUE_DEPTH];
static uint8_t s_cmdq_head[BLE_MAX_SLOTS];
static uint8_t s_cmdq_count[BLE_MAX_SLOTS];
// When the command that is waiting for its answer went out, 0 when none is.
static int64_t s_cmd_sent_us[BLE_MAX_SLOTS];
static SemaphoreHandle_t s_cmdq_lock;
static esp_timer_handle_t s_cmdq_timer;

/** Whether the next command of this slot may go out now. Call with the lock held. */
static bool cmd_gate_open(uint8_t slot, int64_t now)
{
    const int64_t sent = s_cmd_sent_us[slot];
    if (sent == 0) {
        // Nothing outstanding; still leave a pause after the last thing the storage said.
        return now - ble_central_last_notify_us(slot) >= CMD_GAP_US;
    }

    const int64_t replied = ble_central_last_notify_us(slot);
    if (replied >= sent) {
        return now - replied >= CMD_GAP_US;
    }
    return now - sent >= CMD_REPLY_TIMEOUT_US;
}

static void cmd_arm_timer(void)
{
    if (s_cmdq_timer) {
        esp_timer_stop(s_cmdq_timer);
        esp_timer_start_once(s_cmdq_timer, 20 * 1000);
    }
}

/** Send whatever is due; leave the rest for the timer. */
static void cmd_pump(void *arg)
{
    (void) arg;

    bool more = false;
    xSemaphoreTake(s_cmdq_lock, portMAX_DELAY);

    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (s_cmdq_count[slot] == 0) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (!cmd_gate_open(slot, now)) {
            more = true;
            continue;
        }

        queued_cmd_t *cmd = &s_cmdq[slot][s_cmdq_head[slot]];
        s_cmdq_head[slot] = (uint8_t) ((s_cmdq_head[slot] + 1) % CMD_QUEUE_DEPTH);
        s_cmdq_count[slot]--;

        if (ble_central_write(slot, cmd->data, cmd->len, true) == ESP_OK) {
            s_cmd_sent_us[slot] = esp_timer_get_time();
        }

        if (s_cmdq_count[slot] > 0) {
            more = true;
        }
    }

    xSemaphoreGive(s_cmdq_lock);

    if (more) {
        cmd_arm_timer();
    }
}

/** Forget what is queued for a slot, for instance because its link is gone. */
static void cmd_flush(uint8_t slot)
{
    xSemaphoreTake(s_cmdq_lock, portMAX_DELAY);
    s_cmdq_count[slot] = 0;
    s_cmdq_head[slot] = 0;
    s_cmd_sent_us[slot] = 0;
    xSemaphoreGive(s_cmdq_lock);
}

/**
 * Queue an ordinary command for a slot and send it as soon as the storage is ready for it.
 * ESP_ERR_INVALID_STATE when the slot has no link, ESP_ERR_NO_MEM when too many are waiting.
 */
static esp_err_t cmd_enqueue(uint8_t slot, const uint8_t *data, size_t len)
{
    if (!ble_central_is_connected(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    // Longer than any command seen: send it as it is rather than refuse it.
    if (len > CMD_MAX_LEN) {
        return ble_central_write(slot, data, len, true);
    }

    xSemaphoreTake(s_cmdq_lock, portMAX_DELAY);
    if (s_cmdq_count[slot] >= CMD_QUEUE_DEPTH) {
        xSemaphoreGive(s_cmdq_lock);
        ESP_LOGW(TAG, "Slot %u: command queue full, dropping a command", (unsigned) slot);
        return ESP_ERR_NO_MEM;
    }
    queued_cmd_t *slot_entry = &s_cmdq[slot][(s_cmdq_head[slot] + s_cmdq_count[slot]) % CMD_QUEUE_DEPTH];
    slot_entry->len = (uint8_t) len;
    memcpy(slot_entry->data, data, len);
    s_cmdq_count[slot]++;
    xSemaphoreGive(s_cmdq_lock);

    cmd_pump(NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- REST entry point */

esp_err_t ws_bridge_ensure_connected(uint8_t slot, uint32_t connect_timeout_ms)
{
    if (!is_bound(slot)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!ble_central_is_connected(slot)) {
        // Nobody holds this storage right now, which is the normal case for a home automation
        // that sends a command every few hours. Connect for the command and let the usual
        // hold time release it afterwards - the storage takes one connection at a time, so
        // holding it forever would lock the vendor app out.
        xSemaphoreTake(s_connect_wait[slot], 0);
        s_waiting[slot] = true;

        esp_err_t err = ble_central_connect(slot, s_bound[slot].addr, s_bound[slot].type);
        if (err == ESP_OK) {
            xSemaphoreTake(s_connect_wait[slot], pdMS_TO_TICKS(connect_timeout_ms));
        }
        s_waiting[slot] = false;

        if (err != ESP_OK) {
            return err;
        }
        if (!ble_central_is_connected(slot)) {
            return ESP_ERR_TIMEOUT;
        }
    }

    // With a browser on the page the links stay as they are. Without one, restart the countdown
    // so the storage is released the hold time after the last use and not in the middle of it.
    if (client_count() == 0) {
        start_linger();
    }

    return ESP_OK;
}

esp_err_t ws_bridge_command(uint8_t slot, const uint8_t *data, size_t len, uint32_t connect_timeout_ms)
{
    esp_err_t err = ws_bridge_ensure_connected(slot, connect_timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    err = cmd_enqueue(slot, data, len);

    if (client_count() == 0) {
        start_linger();
    }

    return err;
}

int ws_bridge_client_count(void)
{
    return client_count();
}

const char *ws_bridge_slot_name(uint8_t slot)
{
    if (!is_bound(slot)) {
        return "";
    }
    const char *live = ble_central_device_name(slot);
    return live[0] ? live : s_bound[slot].name;
}

const char *ws_bridge_slot_address(uint8_t slot)
{
    return is_bound(slot) ? s_bound[slot].addr : "";
}

/* ------------------------------------------------------------------------- WebSocket -> BLE */

/** The slot a control message is about. Absent means 0, which is also what the old protocol meant. */
static bool message_slot(const cJSON *json, uint8_t *slot)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "slot");
    if (item == NULL) {
        *slot = 0;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble >= BLE_MAX_SLOTS) {
        return false;
    }
    *slot = (uint8_t) item->valuedouble;
    return true;
}

static void handle_control(const char *text, size_t len)
{
    cJSON *json = cJSON_ParseWithLength(text, len);
    if (!json) {
        send_error(BLE_SLOT_NONE, "bad_message", "Not valid JSON");
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "t");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(json);
        send_error(BLE_SLOT_NONE, "bad_message", "Missing message type");
        return;
    }

    uint8_t slot = 0;
    if (!message_slot(json, &slot)) {
        cJSON_Delete(json);
        send_error(BLE_SLOT_NONE, "bad_message", "Unknown slot");
        return;
    }

    if (strcmp(type->valuestring, "connect") == 0) {
        // Answer even when there is nothing to do. A press of connect that produces no reply at
        // all is indistinguishable from a broken bridge, and the browser has no way to tell which
        // it got.
        cancel_linger();

        if (!is_bound(slot)) {
            send_error(slot, "not_bound", "No storage selected yet");
        } else if (ble_central_connect(slot, s_bound[slot].addr, s_bound[slot].type) != ESP_OK) {
            // The reason already went out as an error status carrying the NimBLE code; this is
            // only the coarse signal that the attempt did not get off the ground.
            send_error(slot, "connect_failed", "Could not start connecting");
        }

    } else if (strcmp(type->valuestring, "disconnect") == 0) {
        hold_off_reconnect(slot);
        ble_central_disconnect(slot);

    } else if (strcmp(type->valuestring, "scan") == 0) {
        const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(json, "seconds");
        uint32_t duration = cJSON_IsNumber(seconds) ? (uint32_t) seconds->valuedouble : 5;
        if (duration < 1 || duration > 30) {
            duration = 5;
        }
        if (ble_central_scan(duration, on_scan_finished) != ESP_OK) {
            send_error(BLE_SLOT_NONE, "scan_failed", "Could not start scanning");
        }

    } else if (strcmp(type->valuestring, "bind") == 0) {
        const cJSON *address = cJSON_GetObjectItemCaseSensitive(json, "address");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
        if (!cJSON_IsString(address)) {
            send_error(slot, "bad_message", "bind needs an address");
        } else {
            const int holder = slot_holding(address->valuestring);
            if (holder >= 0 && holder != slot) {
                // Two slots on one storage would fight over its single connection.
                send_error(slot, "already_bound", "That storage is already in another slot");
            } else {
                // A different storage moving in: the old link is no longer the one that is wanted.
                if (!is_bound(slot) || strcasecmp(s_bound[slot].addr, address->valuestring) != 0) {
                    ble_central_disconnect(slot);
                }
                store_binding(slot, address->valuestring, cJSON_IsString(name) ? name->valuestring : NULL);
                ble_central_set_device_name(slot, s_bound[slot].name);
                ESP_LOGI(TAG, "Slot %u bound to %s", (unsigned) slot, s_bound[slot].addr);
                send_status(slot, BLE_STATE_IDLE, NULL);
            }
        }

    } else if (strcmp(type->valuestring, "unbind") == 0) {
        ble_central_disconnect(slot);
        clear_binding(slot);
        ble_central_set_device_name(slot, "");
        ESP_LOGI(TAG, "Slot %u cleared", (unsigned) slot);
        send_status(slot, BLE_STATE_IDLE, NULL);

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

    // Somebody is watching again, so the storages are not going anywhere - but the links they
    // were left on have to be refreshed before they are any use.
    cancel_linger();
    refresh_abandoned_links();

    ESP_LOGI(TAG, "Client connected (fd %d), %d now watching", fd, client_count());

    // Addressed to the newcomer rather than broadcast: the others already know.
    char *hello = hello_json();
    if (hello) {
        send_to(fd, HTTPD_WS_TYPE_TEXT, (const uint8_t *) hello, strlen(hello));
        cJSON_free(hello);
    }
    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (is_bound(slot)) {
            send_status(slot, ble_central_is_connected(slot) ? BLE_STATE_CONNECTED : BLE_STATE_IDLE, NULL);
        }
    }

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

        case HTTPD_WS_TYPE_BINARY: {
            s_frames_in++;
            // Slot, write kind, then at least one byte of payload.
            if (frame.len < 3) {
                break;
            }
            const uint8_t slot = buf[0];
            if (slot >= BLE_MAX_SLOTS) {
                send_error(BLE_SLOT_NONE, "bad_message", "Unknown slot");
                break;
            }
            // The rest goes to the storage untouched. Ordinary commands wait their turn; chunks
            // written without response go straight out.
            const esp_err_t err_write = buf[1] == WRITE_WITH_RESPONSE
                                            ? cmd_enqueue(slot, buf + 2, frame.len - 2)
                                            : ble_central_write(slot, buf + 2, frame.len - 2, false);
            if (err_write != ESP_OK) {
                send_error(slot, "write_failed", err_write == ESP_ERR_NO_MEM ? "Too many commands waiting"
                                                                              : "Not connected to a storage");
            }
            break;
        }

        case HTTPD_WS_TYPE_CLOSE: {
            const int fd = httpd_req_to_sockfd(req);
            drop_client(fd);
            ESP_LOGI(TAG, "Client on fd %d left, %d still watching", fd, client_count());

            // Only once nobody is left, and not straight away. A battery accepts one BLE
            // connection at a time, so holding it after every browser has gone would lock out
            // everything else, including the vendor app - but dropping it while another tab is
            // still using it was the older mistake, and dropping it for the second a reload takes
            // was the one after that.
            if (client_count() == 0) {
                start_linger();
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
 * The heap figures are here for the same reason: how many storages fit is a question for numbers.
 */
void ws_bridge_stats_json(char *out, size_t len)
{
    size_t used = 0;

#define APPEND(...) do { \
        if (used < len) { \
            int n_ = snprintf(out + used, len - used, __VA_ARGS__); \
            if (n_ > 0) used += (size_t) n_; \
        } \
    } while (0)

    APPEND("{\"ble\":[");

    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        ble_stats_t b;
        ble_central_stats(slot, &b);

        APPEND("%s{\"slot\":%u,\"bound\":%s,"
               "\"connected\":%s,\"encrypted\":%s,\"encStatus\":%u,"
               "\"subscribed\":%s,\"subscribedTx\":%s,\"cccdWritten\":%u,"
               "\"mtu\":%u,\"txHandle\":%u,\"rxHandle\":%u,\"cccdHandle\":%u,"
               "\"txProps\":%u,\"rxProps\":%u,"
               "\"notifications\":%u,\"notifyBytes\":%u,"
               "\"writesOk\":%u,\"writesFailed\":%u,"
               "\"writeAcks\":%u,\"writeRejects\":%u,\"lastWriteError\":%u}",
               slot ? "," : "",
               (unsigned) slot,
               is_bound(slot) ? "true" : "false",
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
               (unsigned) b.last_write_error);
    }

    APPEND("],\"ws\":{\"clients\":%d,\"framesIn\":%u,\"sendsOk\":%u,\"sendsFailed\":%u},"
           "\"heap\":{\"free\":%u,\"minFree\":%u,\"largestBlock\":%u}}",
           client_count(),
           (unsigned) s_frames_in,
           (unsigned) s_sends_ok,
           (unsigned) s_sends_failed,
           (unsigned) esp_get_free_heap_size(),
           (unsigned) esp_get_minimum_free_heap_size(),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

#undef APPEND
}

void ws_bridge_session_closed(int fd)
{
    if (!drop_client(fd)) {
        return;
    }

    ESP_LOGI(TAG, "Socket %d gone, %d still watching", fd, client_count());

    if (client_count() == 0) {
        start_linger();
    }
}

esp_err_t ws_bridge_start(httpd_handle_t server)
{
    s_server = server;
    s_send_lock = xSemaphoreCreateMutex();
    if (!s_send_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_cmdq_lock = xSemaphoreCreateMutex();
    if (!s_cmdq_lock) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t cmdq_args = { .callback = &cmd_pump, .name = "ws_cmdq" };
    if (esp_timer_create(&cmdq_args, &s_cmdq_timer) != ESP_OK) {
        return ESP_FAIL;
    }

    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        s_connect_wait[slot] = xSemaphoreCreateBinary();
        if (!s_connect_wait[slot]) {
            return ESP_ERR_NO_MEM;
        }
        s_bound[slot].type = BLE_ADDR_TYPE_UNKNOWN;
    }

    load_bindings();
    load_hold();

    esp_err_t err = ble_central_init(on_ble_notify, on_ble_state);
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t slot = 0; slot < BLE_MAX_SLOTS; slot++) {
        if (s_bound[slot].name[0]) {
            ble_central_set_device_name(slot, s_bound[slot].name);
        }
    }

    const esp_timer_create_args_t keep_args = { .callback = &keep_tick, .name = "ws_keep" };
    if (esp_timer_create(&keep_args, &s_keep_timer) == ESP_OK) {
        esp_timer_start_periodic(s_keep_timer, 1000 * 1000);
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

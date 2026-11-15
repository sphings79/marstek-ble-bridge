#include "rest_api.h"

#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "ble_central.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "wifi.h"
#include "ws_bridge.h"

static const char *TAG = "rest";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_TOKEN_HASH "api_tok_hash"

#define TOKEN_BYTES 32
#define TOKEN_HEX_LEN (TOKEN_BYTES * 2)
#define HASH_LEN 32

// Guessing a 256-bit token is hopeless, but a loop hammering the endpoint still costs a small
// device real work, so failures are throttled the same way logins are.
#define MAX_FAILED_ATTEMPTS 10
#define LOCKOUT_US (60ULL * 1000 * 1000)

// How long a command waits for a storage that nobody is holding to connect. The link attempt
// itself gives up after 10 s, plus service discovery.
#define CONNECT_TIMEOUT_MS 15000

// The same range the web app offers; what a storage does with anything outside it is not known.
#define PEAK_POWER_MIN 0
#define PEAK_POWER_MAX 2500

#define CMD_PEAK_SHAVING 0x29
#define PACKET_MAGIC 0x73
#define PACKET_DIRECTION 0x23

static uint8_t s_token_hash[HASH_LEN];
static bool s_token_enabled = false;

static int s_failed = 0;
static int64_t s_lockout_until_us = 0;

typedef struct {
    bool known;
    bool enabled;
    int power;
    int64_t sent_at_us;
} last_peak_t;

// What was last sent, not what the storage does: it does not report this setting back.
static last_peak_t s_last_peak[BLE_MAX_SLOTS];

// The outcome of the last command that reached the REST interface and passed validation, so a
// failing automation can be looked at from the outside instead of guessed at.
static struct {
    bool known;
    int slot;
    const char *what;
    esp_err_t result;
    int64_t at_us;
} s_last_command;

static void record_command(int slot, const char *what, esp_err_t result)
{
    s_last_command.known = true;
    s_last_command.slot = slot;
    s_last_command.what = what;
    s_last_command.result = result;
    s_last_command.at_us = esp_timer_get_time();
}

static const char *result_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK:               return "ok";
        case ESP_ERR_NOT_FOUND:    return "no storage in slot";
        case ESP_ERR_TIMEOUT:      return "storage did not connect";
        default:                   return "send failed";
    }
}

/* --------------------------------------------------------------------------------- helpers */

static void to_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

/** Comparison whose duration does not depend on where the first difference is. */
static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t) (a[i] ^ b[i]);
    }
    return diff == 0;
}

static void hash_token(const char *token, size_t len, uint8_t out[HASH_LEN])
{
    mbedtls_sha256((const unsigned char *) token, len, out, 0);
}

static void send_json_status(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, body);
}

static void send_error(httpd_req_t *req, const char *status, const char *message)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    send_json_status(req, status, body);
}

static bool read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len <= 0 || req->content_len >= (int) buf_len) {
        return false;
    }

    int received = 0;
    while (received < req->content_len) {
        const int got = httpd_req_recv(req, buf + received, req->content_len - received);
        if (got <= 0) {
            return false;
        }
        received += got;
    }
    buf[received] = '\0';
    return true;
}

/* -------------------------------------------------------------------------------- token store */

static void load_token(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_token_hash);
    if (nvs_get_blob(nvs, NVS_KEY_TOKEN_HASH, s_token_hash, &len) == ESP_OK && len == HASH_LEN) {
        s_token_enabled = true;
        ESP_LOGI(TAG, "API token is set");
    }

    nvs_close(nvs);
}

static esp_err_t store_token_hash(const uint8_t *hash)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (hash) {
        err = nvs_set_blob(nvs, NVS_KEY_TOKEN_HASH, hash, HASH_LEN);
    } else {
        err = nvs_erase_key(nvs, NVS_KEY_TOKEN_HASH);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/* ------------------------------------------------------------------- /api/token (session) */

static esp_err_t token_get(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    send_json_status(req, "200 OK", s_token_enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    return ESP_OK;
}

static esp_err_t token_post(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    uint8_t raw[TOKEN_BYTES];
    esp_fill_random(raw, sizeof(raw));

    char token[TOKEN_HEX_LEN + 1];
    to_hex(raw, sizeof(raw), token);

    uint8_t hash[HASH_LEN];
    hash_token(token, TOKEN_HEX_LEN, hash);

    if (store_token_hash(hash) != ESP_OK) {
        send_error(req, "500 Internal Server Error", "Could not store the token");
        return ESP_OK;
    }

    memcpy(s_token_hash, hash, HASH_LEN);
    s_token_enabled = true;
    s_failed = 0;
    s_lockout_until_us = 0;
    ESP_LOGI(TAG, "A new API token was created");

    // The only time the token exists in readable form.
    char body[TOKEN_HEX_LEN + 24];
    snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token);
    send_json_status(req, "200 OK", body);
    return ESP_OK;
}

static esp_err_t token_delete(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    if (store_token_hash(NULL) != ESP_OK) {
        send_error(req, "500 Internal Server Error", "Could not remove the token");
        return ESP_OK;
    }

    memset(s_token_hash, 0, sizeof(s_token_hash));
    s_token_enabled = false;
    ESP_LOGI(TAG, "The API token was removed");

    send_json_status(req, "200 OK", "{\"enabled\":false}");
    return ESP_OK;
}

/* ----------------------------------------------------------------- token check for commands */

/**
 * Whether the request carries the right bearer token. Sends the refusal itself when it does not,
 * so a handler only carries on when this returns true.
 */
static bool token_guard(httpd_req_t *req)
{
    const int64_t now = esp_timer_get_time();

    if (s_lockout_until_us > now) {
        send_error(req, "429 Too Many Requests", "Too many failed attempts, try again later");
        return false;
    }

    // "Bearer " plus 64 hex characters; anything longer cannot be ours, so a small buffer is the
    // cheapest way to refuse it.
    char header[96];
    bool ok = false;

    if (s_token_enabled &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Bearer ", 7) == 0 && strlen(header + 7) == TOKEN_HEX_LEN) {
        uint8_t presented[HASH_LEN];
        hash_token(header + 7, TOKEN_HEX_LEN, presented);
        ok = equal_ct(presented, s_token_hash, HASH_LEN);
    }

    if (ok) {
        s_failed = 0;
        return true;
    }

    if (++s_failed >= MAX_FAILED_ATTEMPTS) {
        s_failed = 0;
        s_lockout_until_us = now + (int64_t) LOCKOUT_US;
        ESP_LOGW(TAG, "Too many bad API tokens - locked for %d s", (int) (LOCKOUT_US / 1000000));
    }

    // The same answer whether the REST commands are off or the token is wrong: this is not the
    // place to tell a stranger which of the two it is.
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    send_error(req, "401 Unauthorized", "A valid API token is required");
    return false;
}

/* ------------------------------------------------------------- /api/peak-shaving (token) */

/** One command packet in the storage's framing: magic, length, direction, command, payload, XOR. */
static size_t build_packet(uint8_t cmd, const uint8_t *payload, size_t payload_len, uint8_t *out)
{
    const size_t total = 4 + payload_len + 1;

    out[0] = PACKET_MAGIC;
    out[1] = (uint8_t) total;
    out[2] = PACKET_DIRECTION;
    out[3] = cmd;
    memcpy(out + 4, payload, payload_len);

    uint8_t xor_sum = 0;
    for (size_t i = 0; i < total - 1; i++) {
        xor_sum ^= out[i];
    }
    out[total - 1] = xor_sum;

    return total;
}

/**
 * Which storage a request means: by address, by slot, or - when only one is bound - just that one.
 * Sends the refusal itself and returns false when it cannot tell.
 */
static bool resolve_slot(httpd_req_t *req, const cJSON *json, int *out)
{
    const cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(json, "slot");
    const cJSON *address = cJSON_GetObjectItemCaseSensitive(json, "address");

    int slot = -1;
    if (cJSON_IsString(address)) {
        slot = ws_bridge_slot_for_address(address->valuestring);
        if (slot < 0) {
            send_error(req, "404 Not Found", "No slot holds that address");
            return false;
        }
    }
    if (cJSON_IsNumber(slot_item)) {
        const int wanted = (int) slot_item->valuedouble;
        if (wanted < 0 || wanted >= BLE_MAX_SLOTS) {
            send_error(req, "400 Bad Request", "\\\"slot\\\" is out of range");
            return false;
        }
        if (slot >= 0 && slot != wanted) {
            send_error(req, "409 Conflict", "\\\"slot\\\" and \\\"address\\\" name different storages");
            return false;
        }
        slot = wanted;
    }
    if (slot < 0) {
        int only = -1, bound = 0;
        for (int i = 0; i < BLE_MAX_SLOTS; i++) {
            if (ws_bridge_slot_is_bound((uint8_t) i)) {
                only = i;
                bound++;
            }
        }
        if (bound != 1) {
            send_error(req, "400 Bad Request",
                       bound == 0 ? "No storage is bound"
                                  : "Several storages are bound - give \\\"slot\\\" or \\\"address\\\"");
            return false;
        }
        slot = only;
    }

    *out = slot;
    return true;
}

/** Map the outcome of reaching a storage onto an HTTP answer. Returns true when it was a success. */
static bool answer_reach_error(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) {
        return true;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(req, "404 Not Found", "No storage in that slot");
    } else if (err == ESP_ERR_TIMEOUT) {
        send_error(req, "504 Gateway Timeout", "The storage did not connect in time");
    } else {
        send_error(req, "502 Bad Gateway", "The command could not be sent to the storage");
    }
    return false;
}

static esp_err_t peak_shaving_post(httpd_req_t *req)
{
    if (!token_guard(req)) {
        return ESP_OK;
    }

    char body[160];
    if (!read_body(req, body, sizeof(body))) {
        send_error(req, "400 Bad Request", "A small JSON body is required");
        return ESP_OK;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_error(req, "400 Bad Request", "Not valid JSON");
        return ESP_OK;
    }

    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(json, "power");

    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(json);
        send_error(req, "400 Bad Request", "\\\"enabled\\\" must be true or false");
        return ESP_OK;
    }

    // The threshold only matters when peak shaving is on, but the storage takes both values in one
    // command, so it is required then and optional otherwise (0 keeps the call short).
    int power_w = 0;
    if (cJSON_IsNumber(power)) {
        power_w = (int) power->valuedouble;
    } else if (cJSON_IsTrue(enabled)) {
        cJSON_Delete(json);
        send_error(req, "400 Bad Request", "\\\"power\\\" (watts) is required when enabling");
        return ESP_OK;
    }
    if (power_w < PEAK_POWER_MIN || power_w > PEAK_POWER_MAX) {
        cJSON_Delete(json);
        send_error(req, "400 Bad Request", "\\\"power\\\" must be between 0 and 2500 W");
        return ESP_OK;
    }

    int slot = -1;
    if (!resolve_slot(req, json, &slot)) {
        cJSON_Delete(json);
        return ESP_OK;
    }

    const bool turn_on = cJSON_IsTrue(enabled);
    cJSON_Delete(json);

    // peak_state u8, power i16 little-endian.
    const uint8_t payload[3] = {
        turn_on ? 1 : 0,
        (uint8_t) (power_w & 0xff),
        (uint8_t) ((power_w >> 8) & 0xff),
    };
    uint8_t packet[16];
    const size_t packet_len = build_packet(CMD_PEAK_SHAVING, payload, sizeof(payload), packet);

    const esp_err_t err = ws_bridge_command((uint8_t) slot, packet, packet_len, CONNECT_TIMEOUT_MS);
    record_command(slot, "peak-shaving", err);
    if (!answer_reach_error(req, err)) {
        return ESP_OK;
    }

    s_last_peak[slot] = (last_peak_t) {
        .known = true,
        .enabled = turn_on,
        .power = power_w,
        .sent_at_us = esp_timer_get_time(),
    };
    ESP_LOGI(TAG, "Peak shaving %s at %d W sent to slot %d", turn_on ? "on" : "off", power_w, slot);

    char reply[96];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"slot\":%d,\"enabled\":%s,\"power\":%d}", slot,
             turn_on ? "true" : "false", power_w);
    send_json_status(req, "200 OK", reply);
    return ESP_OK;
}

static esp_err_t peak_shaving_get(httpd_req_t *req)
{
    if (!token_guard(req)) {
        return ESP_OK;
    }

    // What the bridge last sent. The storage does not report peak shaving back, so this is as close
    // to a state as there is - and it is empty again after a restart of the bridge.
    char body[512];
    size_t used = (size_t) snprintf(body, sizeof(body), "{\"lastSent\":[");
    const int64_t now = esp_timer_get_time();

    for (int i = 0; i < BLE_MAX_SLOTS; i++) {
        const last_peak_t *p = &s_last_peak[i];
        if (!p->known) {
            used += (size_t) snprintf(body + used, sizeof(body) - used, "%s{\"slot\":%d,\"known\":false}",
                                      i ? "," : "", i);
        } else {
            used += (size_t) snprintf(body + used, sizeof(body) - used,
                                      "%s{\"slot\":%d,\"known\":true,\"enabled\":%s,\"power\":%d,\"ageSeconds\":%lld}",
                                      i ? "," : "", i, p->enabled ? "true" : "false", p->power,
                                      (long long) ((now - p->sent_at_us) / 1000000));
        }
    }
    snprintf(body + used, sizeof(body) - used, "]}");

    send_json_status(req, "200 OK", body);
    return ESP_OK;
}

/* --------------------------------------------------------- /api/status, /api/connection (token) */

/** Copy a string for use inside a JSON string: anything that would break out of it becomes '_'. */
static const char *json_safe(const char *in, char *out, size_t len)
{
    size_t i = 0;
    for (; in[i] && i + 1 < len; i++) {
        const unsigned char c = (unsigned char) in[i];
        out[i] = (c < 0x20 || c == '"' || c == '\\' || c > 0x7e) ? '_' : (char) c;
    }
    out[i] = '\0';
    return out;
}

/**
 * Everything worth polling from outside in one answer: what the bridge is, how it is doing, what
 * each slot holds and what the last REST command did. Meant for a Home Assistant sensor, and for
 * finding out why an automation did nothing.
 */
static esp_err_t status_get(httpd_req_t *req)
{
    if (!token_guard(req)) {
        return ESP_OK;
    }

    static char body[2048];
    size_t used = 0;
#define APPEND(...) do { \
        if (used < sizeof(body)) { \
            int n_ = snprintf(body + used, sizeof(body) - used, __VA_ARGS__); \
            if (n_ > 0) used += (size_t) n_; \
        } \
    } while (0)

    const int64_t now = esp_timer_get_time();
    const esp_app_desc_t *app = esp_app_get_description();

    APPEND("{\"version\":\"%s\",\"protocol\":%d,\"uptimeSeconds\":%lld,\"wifiRssi\":%d,"
           "\"clients\":%d,\"holdSeconds\":%u,"
           "\"heap\":{\"free\":%u,\"minFree\":%u,\"largestBlock\":%u},\"slots\":[",
           app->version, WS_BRIDGE_PROTOCOL_VERSION, (long long) (now / 1000000), (int) wifi_rssi(),
           ws_bridge_client_count(), (unsigned) ws_bridge_hold_seconds(),
           (unsigned) esp_get_free_heap_size(), (unsigned) esp_get_minimum_free_heap_size(),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    for (int i = 0; i < BLE_MAX_SLOTS; i++) {
        const bool bound = ws_bridge_slot_is_bound((uint8_t) i);
        const bool connected = ble_central_is_connected((uint8_t) i);
        const last_peak_t *p = &s_last_peak[i];
        char name[BLE_NAME_MAX + 1], address[BLE_ADDR_STR_LEN + 1];

        APPEND("%s{\"slot\":%d,\"bound\":%s,\"name\":\"%s\",\"address\":\"%s\","
               "\"connected\":%s,\"rssi\":%d,\"mtu\":%u,"
               "\"keepConnected\":%s,\"nextAttemptSeconds\":",
               i ? "," : "", i, bound ? "true" : "false", json_safe(ws_bridge_slot_name((uint8_t) i), name, sizeof(name)),
               json_safe(ws_bridge_slot_address((uint8_t) i), address, sizeof(address)), connected ? "true" : "false",
               connected ? (int) ble_central_rssi((uint8_t) i) : 0,
               connected ? (unsigned) ble_central_mtu((uint8_t) i) : 0u,
               ws_bridge_keep((uint8_t) i) ? "true" : "false");

        const int next_attempt = ws_bridge_keep_next_attempt_seconds((uint8_t) i);
        if (next_attempt >= 0) {
            APPEND("%d", next_attempt);
        } else {
            APPEND("null");
        }
        APPEND(",\"lastSent\":");

        if (p->known) {
            APPEND("{\"peakShaving\":%s,\"power\":%d,\"ageSeconds\":%lld}",
                   p->enabled ? "true" : "false", p->power, (long long) ((now - p->sent_at_us) / 1000000));
        } else {
            APPEND("null");
        }
        APPEND("}");
    }

    APPEND("],\"lastCommand\":");
    if (s_last_command.known) {
        APPEND("{\"what\":\"%s\",\"slot\":%d,\"ok\":%s,\"result\":\"%s\",\"ageSeconds\":%lld}",
               s_last_command.what, s_last_command.slot,
               s_last_command.result == ESP_OK ? "true" : "false", result_name(s_last_command.result),
               (long long) ((now - s_last_command.at_us) / 1000000));
    } else {
        APPEND("null");
    }
    APPEND("}");
#undef APPEND

    send_json_status(req, "200 OK", body);
    return ESP_OK;
}

/**
 * Connect or disconnect a storage on request: a way to test that the bridge reaches it, and how
 * long it holds it, without sending anything the storage would act on.
 */
static esp_err_t connection_post(httpd_req_t *req)
{
    if (!token_guard(req)) {
        return ESP_OK;
    }

    char body[160];
    if (!read_body(req, body, sizeof(body))) {
        send_error(req, "400 Bad Request", "A small JSON body is required");
        return ESP_OK;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_error(req, "400 Bad Request", "Not valid JSON");
        return ESP_OK;
    }

    const cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    const bool connect = cJSON_IsString(action) && strcmp(action->valuestring, "connect") == 0;
    const bool disconnect = cJSON_IsString(action) && strcmp(action->valuestring, "disconnect") == 0;
    if (!connect && !disconnect) {
        cJSON_Delete(json);
        send_error(req, "400 Bad Request", "\\\"action\\\" must be connect or disconnect");
        return ESP_OK;
    }

    int slot = -1;
    if (!resolve_slot(req, json, &slot)) {
        cJSON_Delete(json);
        return ESP_OK;
    }
    cJSON_Delete(json);

    if (disconnect) {
        ws_bridge_note_manual_disconnect((uint8_t) slot);
        ble_central_disconnect((uint8_t) slot);
        record_command(slot, "disconnect", ESP_OK);
    } else {
        const esp_err_t err = ws_bridge_ensure_connected((uint8_t) slot, CONNECT_TIMEOUT_MS);
        record_command(slot, "connect", err);
        if (!answer_reach_error(req, err)) {
            return ESP_OK;
        }
    }

    char reply[96];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"slot\":%d,\"action\":\"%s\"}", slot,
             connect ? "connect" : "disconnect");
    send_json_status(req, "200 OK", reply);
    return ESP_OK;
}

/* ------------------------------------------------------------------- /api/settings (session) */

static void write_settings(httpd_req_t *req)
{
    char body[96];
    size_t used = (size_t) snprintf(body, sizeof(body), "{\"holdSeconds\":%u,\"keepConnected\":[",
                                    (unsigned) ws_bridge_hold_seconds());
    for (int i = 0; i < BLE_MAX_SLOTS; i++) {
        used += (size_t) snprintf(body + used, sizeof(body) - used, "%s%s", i ? "," : "",
                                  ws_bridge_keep((uint8_t) i) ? "true" : "false");
    }
    snprintf(body + used, sizeof(body) - used, "]}");
    send_json_status(req, "200 OK", body);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    write_settings(req);
    return ESP_OK;
}

/** Either or both of {"holdSeconds":n} and {"keep":{"slot":n,"enabled":bool}}; the answer is the new state. */
static esp_err_t settings_post(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        send_error(req, "400 Bad Request", "A small JSON body is required");
        return ESP_OK;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_error(req, "400 Bad Request", "Not valid JSON");
        return ESP_OK;
    }

    const cJSON *hold = cJSON_GetObjectItemCaseSensitive(json, "holdSeconds");
    const cJSON *keep = cJSON_GetObjectItemCaseSensitive(json, "keep");

    // Check everything before changing anything, so a bad request leaves the settings as they were.
    int hold_seconds = 0;
    if (hold) {
        if (!cJSON_IsNumber(hold) || hold->valuedouble < 5 || hold->valuedouble > 3600 ||
            hold->valuedouble != (double) (int) hold->valuedouble) {
            cJSON_Delete(json);
            send_error(req, "400 Bad Request", "\\\"holdSeconds\\\" must be a whole number from 5 to 3600");
            return ESP_OK;
        }
        hold_seconds = (int) hold->valuedouble;
    }

    int keep_slot = -1;
    bool keep_enabled = false;
    if (keep) {
        const cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(keep, "slot");
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(keep, "enabled");
        if (!cJSON_IsNumber(slot_item) || slot_item->valuedouble < 0 || slot_item->valuedouble >= BLE_MAX_SLOTS ||
            !cJSON_IsBool(enabled)) {
            cJSON_Delete(json);
            send_error(req, "400 Bad Request", "\\\"keep\\\" needs a valid \\\"slot\\\" and \\\"enabled\\\"");
            return ESP_OK;
        }
        keep_slot = (int) slot_item->valuedouble;
        keep_enabled = cJSON_IsTrue(enabled);
        if (keep_enabled && !ws_bridge_slot_is_bound((uint8_t) keep_slot)) {
            cJSON_Delete(json);
            send_error(req, "409 Conflict", "Nothing is bound to that slot");
            return ESP_OK;
        }
    }
    cJSON_Delete(json);

    if (!hold && !keep) {
        send_error(req, "400 Bad Request", "Nothing to change");
        return ESP_OK;
    }

    if (hold && ws_bridge_set_hold_seconds((uint16_t) hold_seconds) != ESP_OK) {
        send_error(req, "500 Internal Server Error", "Could not store the setting");
        return ESP_OK;
    }
    if (keep && ws_bridge_set_keep((uint8_t) keep_slot, keep_enabled) != ESP_OK) {
        send_error(req, "500 Internal Server Error", "Could not store the setting");
        return ESP_OK;
    }

    write_settings(req);
    return ESP_OK;
}

/* ------------------------------------------------------------------------------ registration */

esp_err_t rest_api_register_handlers(httpd_handle_t server)
{
    load_token();

    const httpd_uri_t routes[] = {
        { .uri = "/api/token",        .method = HTTP_GET,    .handler = token_get },
        { .uri = "/api/token",        .method = HTTP_POST,   .handler = token_post },
        { .uri = "/api/token",        .method = HTTP_DELETE, .handler = token_delete },
        { .uri = "/api/peak-shaving", .method = HTTP_POST,   .handler = peak_shaving_post },
        { .uri = "/api/peak-shaving", .method = HTTP_GET,    .handler = peak_shaving_get },
        { .uri = "/api/status",       .method = HTTP_GET,    .handler = status_get },
        { .uri = "/api/connection",   .method = HTTP_POST,   .handler = connection_post },
        { .uri = "/api/settings",     .method = HTTP_GET,    .handler = settings_get },
        { .uri = "/api/settings",     .method = HTTP_POST,   .handler = settings_post },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

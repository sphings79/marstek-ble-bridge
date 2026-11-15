#include "rest_api.h"

#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "ble_central.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
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
    const cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(json, "slot");
    const cJSON *address = cJSON_GetObjectItemCaseSensitive(json, "address");

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

    // Which storage: by address, by slot, or - when only one is bound - just that one.
    int slot = -1;
    if (cJSON_IsString(address)) {
        slot = ws_bridge_slot_for_address(address->valuestring);
        if (slot < 0) {
            cJSON_Delete(json);
            send_error(req, "404 Not Found", "No slot holds that address");
            return ESP_OK;
        }
    }
    if (cJSON_IsNumber(slot_item)) {
        const int wanted = (int) slot_item->valuedouble;
        if (wanted < 0 || wanted >= BLE_MAX_SLOTS) {
            cJSON_Delete(json);
            send_error(req, "400 Bad Request", "\\\"slot\\\" is out of range");
            return ESP_OK;
        }
        if (slot >= 0 && slot != wanted) {
            cJSON_Delete(json);
            send_error(req, "409 Conflict", "\\\"slot\\\" and \\\"address\\\" name different storages");
            return ESP_OK;
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
            cJSON_Delete(json);
            send_error(req, "400 Bad Request",
                       bound == 0 ? "No storage is bound" : "Several storages are bound - give \\\"slot\\\" or \\\"address\\\"");
            return ESP_OK;
        }
        slot = only;
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
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(req, "404 Not Found", "No storage in that slot");
        return ESP_OK;
    }
    if (err == ESP_ERR_TIMEOUT) {
        send_error(req, "504 Gateway Timeout", "The storage did not connect in time");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        send_error(req, "502 Bad Gateway", "The command could not be sent to the storage");
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
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

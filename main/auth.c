#include "auth.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "auth";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_SALT  "pw_salt"
#define NVS_KEY_KEY   "pw_key"
#define NVS_KEY_SESSION "session"

#define SALT_LEN 16
#define KEY_LEN 32
#define NONCE_LEN 16
#define SESSION_LEN 32

#define NONCE_LIFETIME_US (60ULL * 1000 * 1000)
#define SESSION_LIFETIME_US (12ULL * 60 * 60 * 1000 * 1000)

// Failed logins are cheap to attempt and the device is small, so a burst is throttled rather than
// left to run. Long enough to make guessing pointless, short enough that a typo is not punished.
#define MAX_FAILED_ATTEMPTS 5
#define LOCKOUT_US (60ULL * 1000 * 1000)

static uint8_t s_salt[SALT_LEN];
static uint8_t s_key[KEY_LEN];
static bool s_claimed = false;

static uint8_t s_nonce[NONCE_LEN];
static bool s_nonce_valid = false;
static int64_t s_nonce_issued_us = 0;

static char s_session[SESSION_LEN * 2 + 1];
static bool s_session_valid = false;
static int64_t s_session_issued_us = 0;

static int s_failed = 0;
static int64_t s_lockout_until_us = 0;

/**
 * Keep the session across a restart.
 *
 * Every firmware or interface update reboots the bridge, and a session held only in RAM dies with
 * it - so the browser still holds a cookie the bridge no longer recognises, and the next request
 * is refused with no obvious reason. Updating firmware then means logging in between every step,
 * which is exactly the friction the remote update was meant to remove.
 *
 * The token is no more sensitive than the password hash already stored beside it, and it is what
 * the browser is holding anyway.
 */
static void persist_session(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    if (s_session_valid) {
        nvs_set_str(nvs, NVS_KEY_SESSION, s_session);
    } else {
        nvs_erase_key(nvs, NVS_KEY_SESSION);
    }

    nvs_commit(nvs);
    nvs_close(nvs);
}

static void to_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static bool from_hex(const char *hex, uint8_t *out, size_t len)
{
    if (!hex || strlen(hex) != len * 2) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        int hi = -1, lo = -1;
        char a = hex[i * 2], b = hex[i * 2 + 1];

        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;

        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;

        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t) ((hi << 4) | lo);
    }

    return true;
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

esp_err_t auth_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No credentials stored - bridge is unclaimed");
        return ESP_OK;
    }

    size_t salt_len = sizeof(s_salt);
    size_t key_len = sizeof(s_key);

    if (nvs_get_blob(nvs, NVS_KEY_SALT, s_salt, &salt_len) == ESP_OK && salt_len == SALT_LEN &&
        nvs_get_blob(nvs, NVS_KEY_KEY, s_key, &key_len) == ESP_OK && key_len == KEY_LEN) {
        s_claimed = true;
        ESP_LOGI(TAG, "Password is set");
    } else {
        ESP_LOGI(TAG, "No credentials stored - bridge is unclaimed");
    }

    size_t session_len = sizeof(s_session);
    if (nvs_get_str(nvs, NVS_KEY_SESSION, s_session, &session_len) == ESP_OK &&
        session_len == sizeof(s_session)) {
        s_session_valid = true;
        // Uptime is all we have to measure age by, and it starts again at zero, so a restored
        // session gets a fresh window rather than an unknowable one.
        s_session_issued_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Session restored from before the restart");
    }

    nvs_close(nvs);
    return ESP_OK;
}

bool auth_is_claimed(void)
{
    return s_claimed;
}

/**
 * Claiming is only possible shortly after boot.
 *
 * A freshly flashed bridge has no password, and whoever reaches it first sets one. Bounding that
 * to the first few minutes means an unclaimed bridge left powered on for a week is not standing
 * open the whole time - taking it over then requires physical access to power-cycle it.
 */
static bool claim_window_open(void)
{
    return esp_timer_get_time() < (int64_t) CONFIG_BRIDGE_CLAIM_WINDOW_MINUTES * 60 * 1000000;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }

    buf[received] = '\0';
    return ESP_OK;
}

static void send_status(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
}

/** Hand out the salt to derive the key with, plus a single-use nonce to answer. */
static esp_err_t challenge_get(httpd_req_t *req)
{
    // An unclaimed bridge has no salt yet; give the client a fresh one to claim with.
    if (!s_claimed) {
        esp_fill_random(s_salt, sizeof(s_salt));
    }

    esp_fill_random(s_nonce, sizeof(s_nonce));
    s_nonce_valid = true;
    s_nonce_issued_us = esp_timer_get_time();

    char salt_hex[SALT_LEN * 2 + 1];
    char nonce_hex[NONCE_LEN * 2 + 1];
    to_hex(s_salt, sizeof(s_salt), salt_hex);
    to_hex(s_nonce, sizeof(s_nonce), nonce_hex);

    char body[128];
    snprintf(body, sizeof(body), "{\"salt\":\"%s\",\"nonce\":\"%s\"}", salt_hex, nonce_hex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t claim_post(httpd_req_t *req)
{
    if (s_claimed) {
        send_status(req, "409 Conflict");
        return ESP_OK;
    }

    if (!claim_window_open()) {
        ESP_LOGW(TAG, "Claim attempt after the window closed - power-cycle to reopen it");
        send_status(req, "403 Forbidden");
        return ESP_OK;
    }

    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    const cJSON *salt_item = cJSON_GetObjectItemCaseSensitive(json, "salt");
    const cJSON *key_item = cJSON_GetObjectItemCaseSensitive(json, "key");

    uint8_t salt[SALT_LEN];
    uint8_t key[KEY_LEN];

    if (!cJSON_IsString(salt_item) || !cJSON_IsString(key_item) ||
        !from_hex(salt_item->valuestring, salt, sizeof(salt)) ||
        !from_hex(key_item->valuestring, key, sizeof(key))) {
        cJSON_Delete(json);
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }
    cJSON_Delete(json);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        send_status(req, "500 Internal Server Error");
        return ESP_OK;
    }

    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_SALT, salt, sizeof(salt));
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY_KEY, key, sizeof(key));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Storing credentials failed: %s", esp_err_to_name(err));
        send_status(req, "500 Internal Server Error");
        return ESP_OK;
    }

    memcpy(s_salt, salt, sizeof(salt));
    memcpy(s_key, key, sizeof(key));
    s_claimed = true;

    ESP_LOGI(TAG, "Bridge claimed");
    send_status(req, "204 No Content");
    return ESP_OK;
}

static esp_err_t login_post(httpd_req_t *req)
{
    const int64_t now = esp_timer_get_time();

    if (now < s_lockout_until_us) {
        send_status(req, "429 Too Many Requests");
        return ESP_OK;
    }

    if (!s_claimed || !s_nonce_valid || (now - s_nonce_issued_us) > (int64_t) NONCE_LIFETIME_US) {
        send_status(req, "401 Unauthorized");
        return ESP_OK;
    }

    char body[256];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    const cJSON *nonce_item = cJSON_GetObjectItemCaseSensitive(json, "nonce");
    const cJSON *response_item = cJSON_GetObjectItemCaseSensitive(json, "response");

    uint8_t claimed_nonce[NONCE_LEN];
    uint8_t offered[KEY_LEN];

    bool parsed = cJSON_IsString(nonce_item) && cJSON_IsString(response_item) &&
                  from_hex(nonce_item->valuestring, claimed_nonce, sizeof(claimed_nonce)) &&
                  from_hex(response_item->valuestring, offered, sizeof(offered));
    cJSON_Delete(json);

    if (!parsed) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    // Whatever happens next, this nonce is spent. Otherwise a captured response could be replayed
    // for as long as the nonce lived.
    s_nonce_valid = false;

    if (!equal_ct(claimed_nonce, s_nonce, sizeof(s_nonce))) {
        send_status(req, "401 Unauthorized");
        return ESP_OK;
    }

    uint8_t expected[KEY_LEN];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, s_key, sizeof(s_key), s_nonce, sizeof(s_nonce), expected) != 0) {
        send_status(req, "500 Internal Server Error");
        return ESP_OK;
    }

    if (!equal_ct(offered, expected, sizeof(expected))) {
        if (++s_failed >= MAX_FAILED_ATTEMPTS) {
            ESP_LOGW(TAG, "Too many failed logins, locking out for a minute");
            s_lockout_until_us = now + (int64_t) LOCKOUT_US;
            s_failed = 0;
        }
        send_status(req, "401 Unauthorized");
        return ESP_OK;
    }

    s_failed = 0;

    uint8_t session[SESSION_LEN];
    esp_fill_random(session, sizeof(session));
    to_hex(session, sizeof(session), s_session);
    s_session_valid = true;
    s_session_issued_us = now;
    persist_session();

    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             "bridge_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%llu",
             s_session, SESSION_LIFETIME_US / 1000000);

    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    ESP_LOGI(TAG, "Login accepted");
    send_status(req, "204 No Content");
    return ESP_OK;
}

/**
 * Change the password.
 *
 * Wants three things: a valid session, a fresh nonce answered with the *old* key, and the salt and
 * key derived from the new password. The middle one is the point. A session cookie is readable on
 * the wire, and this is the one request where that would matter beyond eavesdropping - it would
 * let whoever caught it lock the owner out of their own bridge. Proving the old password first
 * makes a stolen cookie insufficient.
 *
 * The new key is derived in the browser, exactly as when claiming, so neither password ever
 * crosses the network.
 */
static esp_err_t password_post(httpd_req_t *req)
{
    const int64_t now = esp_timer_get_time();

    if (now < s_lockout_until_us) {
        send_status(req, "429 Too Many Requests");
        return ESP_OK;
    }

    if (!auth_guard(req)) {
        return ESP_OK;
    }

    if (!s_nonce_valid || (now - s_nonce_issued_us) > (int64_t) NONCE_LIFETIME_US) {
        send_status(req, "401 Unauthorized");
        return ESP_OK;
    }

    char body[384];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    const cJSON *nonce_item = cJSON_GetObjectItemCaseSensitive(json, "nonce");
    const cJSON *response_item = cJSON_GetObjectItemCaseSensitive(json, "response");
    const cJSON *salt_item = cJSON_GetObjectItemCaseSensitive(json, "salt");
    const cJSON *key_item = cJSON_GetObjectItemCaseSensitive(json, "key");

    uint8_t claimed_nonce[NONCE_LEN];
    uint8_t offered[KEY_LEN];
    uint8_t salt[SALT_LEN];
    uint8_t key[KEY_LEN];

    const bool parsed =
        cJSON_IsString(nonce_item) && cJSON_IsString(response_item) &&
        cJSON_IsString(salt_item) && cJSON_IsString(key_item) &&
        from_hex(nonce_item->valuestring, claimed_nonce, sizeof(claimed_nonce)) &&
        from_hex(response_item->valuestring, offered, sizeof(offered)) &&
        from_hex(salt_item->valuestring, salt, sizeof(salt)) &&
        from_hex(key_item->valuestring, key, sizeof(key));
    cJSON_Delete(json);

    if (!parsed) {
        send_status(req, "400 Bad Request");
        return ESP_OK;
    }

    // Spent either way, as in login: a captured response must not survive its own use.
    s_nonce_valid = false;

    uint8_t expected[KEY_LEN];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (!equal_ct(claimed_nonce, s_nonce, sizeof(s_nonce)) ||
        mbedtls_md_hmac(md, s_key, sizeof(s_key), s_nonce, sizeof(s_nonce), expected) != 0 ||
        !equal_ct(offered, expected, sizeof(expected))) {
        if (++s_failed >= MAX_FAILED_ATTEMPTS) {
            ESP_LOGW(TAG, "Too many failed password changes, locking out for a minute");
            s_lockout_until_us = now + (int64_t) LOCKOUT_US;
            s_failed = 0;
        }
        send_status(req, "401 Unauthorized");
        return ESP_OK;
    }

    s_failed = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        send_status(req, "500 Internal Server Error");
        return ESP_OK;
    }

    esp_err_t err = nvs_set_blob(nvs, NVS_KEY_SALT, salt, sizeof(salt));
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY_KEY, key, sizeof(key));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Storing the new password failed: %s", esp_err_to_name(err));
        send_status(req, "500 Internal Server Error");
        return ESP_OK;
    }

    memcpy(s_salt, salt, sizeof(salt));
    memcpy(s_key, key, sizeof(key));

    // Every session dies with the old password, this one included. Someone changing their password
    // because a session may have leaked would gain nothing if the leaked session outlived it.
    s_session_valid = false;
    persist_session();

    httpd_resp_set_hdr(req, "Set-Cookie",
                       "bridge_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");

    ESP_LOGI(TAG, "Password changed - all sessions ended");
    send_status(req, "204 No Content");
    return ESP_OK;
}

static esp_err_t logout_post(httpd_req_t *req)
{
    s_session_valid = false;
    persist_session();

    httpd_resp_set_hdr(req, "Set-Cookie", "bridge_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    send_status(req, "204 No Content");
    return ESP_OK;
}

bool auth_request_is_authenticated(httpd_req_t *req)
{
    if (!s_session_valid) {
        return false;
    }

    if ((esp_timer_get_time() - s_session_issued_us) > (int64_t) SESSION_LIFETIME_US) {
        s_session_valid = false;
        return false;
    }

    char cookies[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof(cookies)) != ESP_OK) {
        return false;
    }

    const char *found = strstr(cookies, "bridge_session=");
    if (!found) {
        return false;
    }
    found += strlen("bridge_session=");

    const size_t expected_len = strlen(s_session);
    if (strlen(found) < expected_len) {
        return false;
    }

    // Reject a longer token that merely starts with ours.
    const char terminator = found[expected_len];
    if (terminator != '\0' && terminator != ';' && terminator != ' ') {
        return false;
    }

    return equal_ct((const uint8_t *) found, (const uint8_t *) s_session, expected_len);
}

bool auth_guard(httpd_req_t *req)
{
    if (auth_request_is_authenticated(req)) {
        return true;
    }

    send_status(req, "401 Unauthorized");
    return false;
}

esp_err_t auth_register_handlers(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/api/auth/challenge", .method = HTTP_GET,  .handler = challenge_get },
        { .uri = "/api/auth/claim",     .method = HTTP_POST, .handler = claim_post },
        { .uri = "/api/auth/login",     .method = HTTP_POST, .handler = login_post },
        { .uri = "/api/auth/logout",    .method = HTTP_POST, .handler = logout_post },
        { .uri = "/api/auth/password",  .method = HTTP_POST, .handler = password_post },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

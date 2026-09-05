#include "wifi_setup.h"

#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "cJSON.h"
#include "esp_log.h"
#include "wifi.h"

static const char *TAG = "wifi_setup";

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

/**
 * Who may change the network.
 *
 * An unclaimed bridge lets anyone through, because there is no password to check yet and refusing
 * would leave a freshly flashed board with no way to be set up at all. That is the same window
 * claiming itself relies on, and it closes the moment a password is set.
 */
static bool setup_allowed(httpd_req_t *req)
{
    if (!auth_is_claimed()) {
        return true;
    }
    return auth_guard(req);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char body[256];
    snprintf(body, sizeof(body),
             "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":%s,"
             "\"configured\":%s,\"hostname\":\"%s\"}",
             wifi_is_connected() ? "true" : "false",
             wifi_ssid(),
             wifi_ip(),
             wifi_fallback_ap_active() ? "true" : "false",
             wifi_has_credentials() ? "true" : "false",
             wifi_hostname());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!setup_allowed(req)) {
        return ESP_OK;
    }

    wifi_scan_entry_t networks[16];
    const size_t found = wifi_scan(networks, 16);

    cJSON *array = cJSON_CreateArray();
    if (!array) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    for (size_t i = 0; i < found; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(entry, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(entry, "secured", networks[i].secured);
        cJSON_AddItemToArray(array, entry);
    }

    char *body = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);

    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);

    return err;
}

static esp_err_t credentials_post(httpd_req_t *req)
{
    if (!setup_allowed(req)) {
        return ESP_OK;
    }

    char raw[256];
    if (req->content_len >= (int) sizeof(raw)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        const int got = httpd_req_recv(req, raw + received, req->content_len - received);
        if (got <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Truncated body");
            return ESP_FAIL;
        }
        received += got;
    }
    raw[received] = '\0';

    cJSON *json = cJSON_Parse(raw);
    const cJSON *ssid = json ? cJSON_GetObjectItemCaseSensitive(json, "ssid") : NULL;
    const cJSON *password = json ? cJSON_GetObjectItemCaseSensitive(json, "password") : NULL;

    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "A network name is required");
        return ESP_FAIL;
    }

    char want_ssid[WIFI_SSID_MAX];
    char want_password[WIFI_PASSWORD_MAX];
    strlcpy(want_ssid, ssid->valuestring, sizeof(want_ssid));
    strlcpy(want_password, cJSON_IsString(password) ? password->valuestring : "",
            sizeof(want_password));
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Asked to join \"%s\"", want_ssid);

    // Answering only once the join has been tried is the point: the browser making this request
    // is very often on the bridge's own access point, which the bridge is about to leave. A
    // reply that arrives is worth waiting for, and a failure that arrives is worth far more.
    const esp_err_t err = wifi_provision(want_ssid, want_password);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (err != ESP_OK) {
        char body[128];
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"Could not join that network - check the password\"}");
        return httpd_resp_sendstr(req, body);
    }

    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":true,\"ip\":\"%s\"}", wifi_ip());
    return httpd_resp_sendstr(req, body);
}

/**
 * Rename the bridge.
 *
 * Its own endpoint rather than a field on the credentials form: renaming is the thing you do when
 * a second bridge arrives, long after the first was set up, and it must not require retyping a
 * WiFi password to get there.
 */
static esp_err_t hostname_post(httpd_req_t *req)
{
    if (!setup_allowed(req)) {
        return ESP_OK;
    }

    char raw[160];
    if (req->content_len >= (int) sizeof(raw)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        const int got = httpd_req_recv(req, raw + received, req->content_len - received);
        if (got <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Truncated body");
            return ESP_FAIL;
        }
        received += got;
    }
    raw[received] = '\0';

    cJSON *json = cJSON_Parse(raw);
    const cJSON *name = json ? cJSON_GetObjectItemCaseSensitive(json, "hostname") : NULL;

    if (!cJSON_IsString(name)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "A name is required");
        return ESP_FAIL;
    }

    const esp_err_t err = wifi_set_hostname(name->valuestring);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"Lower-case letters, digits and inner hyphens only\"}");
    }
    if (err != ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Could not store the name\"}");
    }

    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"hostname\":\"%s\"}", wifi_hostname());
    return httpd_resp_sendstr(req, body);
}

static esp_err_t forget_post(httpd_req_t *req)
{
    // Never unauthenticated: forgetting the network on a claimed bridge would strand it behind an
    // access point somebody has to walk to.
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    wifi_forget();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);
}

esp_err_t wifi_setup_register_handlers(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        { .uri = "/setup",          .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/wifi",       .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/wifi",       .method = HTTP_POST, .handler = credentials_post },
        { .uri = "/api/wifi/scan",  .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/wifi/forget",.method = HTTP_POST, .handler = forget_post },
        { .uri = "/api/wifi/hostname", .method = HTTP_POST, .handler = hostname_post },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

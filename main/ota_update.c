#include "ota_update.h"

#include "auth.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "ble_central.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ws_bridge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define CHUNK 2048

static char s_buf[CHUNK];

static void reboot_soon(void *arg)
{
    (void) arg;
    ESP_LOGI(TAG, "Restarting into the new firmware");
    esp_restart();
}

/**
 * Take a firmware image and put it in the slot the bridge is not running from.
 *
 * The point of this endpoint is that the bridge sits wherever the storage sits - a cellar, a
 * utility room - and fetching a cable for every change costs more than the change. Nothing is
 * committed until the whole image has arrived and passed validation, so a transfer that dies
 * halfway leaves the running firmware untouched.
 */
static esp_err_t update_post(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No free slot");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving %d bytes into %s", req->content_len, target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        const int want = remaining < CHUNK ? remaining : CHUNK;
        const int got = httpd_req_recv(req, s_buf, want);

        if (got <= 0) {
            // Abort rather than commit a partial image. The slot is rewritten from scratch next
            // time, and the firmware currently running is untouched either way.
            ESP_LOGE(TAG, "Transfer broke off with %d bytes to go", remaining);
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Transfer interrupted");
            return ESP_FAIL;
        }

        err = esp_ota_write(handle, s_buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }

        remaining -= got;
    }

    // Checks the image header and its checksum. A truncated or corrupt upload is refused here,
    // before anything points the bootloader at it.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image rejected: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot switch slots: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");

    // Give the response time to leave before the network goes away with the reboot.
    const esp_timer_create_args_t args = { .callback = &reboot_soon, .name = "ota_reboot" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 500 * 1000);
    }

    return ESP_OK;
}

/**
 * Replace the web partition, which holds the interface the bridge serves.
 *
 * Without this the firmware could be updated over the network but the app it serves could not, so
 * every change to the interface would still mean fetching a cable - which is most of what this
 * endpoint exists to avoid.
 *
 * Unlike the app slots there is no spare copy to write into: the partition is erased first, and
 * an upload that breaks off leaves the bridge without an interface until the next attempt. The API
 * keeps working throughout, so a retry is always possible.
 */
static esp_err_t update_web_post(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    const esp_partition_t *web = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "web");
    if (!web) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No web partition");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || (size_t) req->content_len > web->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Image does not fit the web partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Replacing the web partition with %d bytes", req->content_len);

    // Unmount first: writing underneath a mounted filesystem would leave its cached state lying
    // about something that no longer exists.
    esp_vfs_spiffs_unregister("web");

    esp_err_t err = esp_partition_erase_range(web, 0, web->size);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    int offset = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        const int want = remaining < CHUNK ? remaining : CHUNK;
        const int got = httpd_req_recv(req, s_buf, want);
        if (got <= 0) {
            ESP_LOGE(TAG, "Transfer broke off with %d bytes to go", remaining);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Transfer interrupted");
            return ESP_FAIL;
        }

        err = esp_partition_write(web, offset, s_buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write failed at %d: %s", offset, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }

        offset += got;
        remaining -= got;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");

    // Simpler and safer than remounting underneath a running server.
    const esp_timer_create_args_t args = { .callback = &reboot_soon, .name = "web_reboot" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 500 * 1000);
    }

    return ESP_OK;
}

/** What is running right now, so an update can be checked rather than assumed. */
static esp_err_t version_get(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    char body[256];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\",\"slot\":\"%s\"}",
             app->version, app->date, app->time, app->idf_ver,
             running ? running->label : "?");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/**
 * Fetch an image over HTTPS and install it, rather than having it pushed through the browser.
 *
 * This is what release updates use. The alternative - the browser downloading the asset and
 * forwarding it - founders on GitHub's release assets, which carry no cross-origin headers, and
 * would push a megabyte through the page for no reason.
 *
 * The BLE link is dropped first. TLS wants tens of kilobytes of heap on a chip that is already
 * running WiFi and a Bluetooth stack, and an update that runs out of memory halfway is a poor
 * trade for keeping a connection that reconnects by itself.
 */
static esp_err_t fetch_firmware(const char *url, char *err, size_t err_len)
{
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        // A release download redirects to a signed storage url that runs to several hundred
        // characters. The default half-kilobyte buffers cannot hold the request line that
        // follows, and the connection fails before anything is sent.
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_err_t result = esp_https_ota(&cfg);
    if (result != ESP_OK) {
        snprintf(err, err_len, "%s", esp_err_to_name(result));
    }

    return result;
}

static esp_err_t fetch_web(const char *url, char *err, size_t err_len)
{
    const esp_partition_t *web = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "web");
    if (!web) {
        snprintf(err, err_len, "no web partition");
        return ESP_FAIL;
    }

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        // As above: room for the redirected url a release download lands on.
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) {
        snprintf(err, err_len, "client init failed");
        return ESP_FAIL;
    }

    // Opening the connection by hand means redirects are ours to follow, and release downloads
    // are nothing but redirects: GitHub answers every one of them with a 302 to a storage host.
    // Five hops is far more than that needs and still terminates.
    esp_err_t result = ESP_FAIL;
    int total = 0;
    int status = 0;

    for (int hop = 0; hop < 5; hop++) {
        result = esp_http_client_open(client, 0);
        if (result != ESP_OK) {
            snprintf(err, err_len, "connect: %s", esp_err_to_name(result));
            esp_http_client_cleanup(client);
            return result;
        }

        total = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);

        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            break;
        }

        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        status = 0;
    }

    if (status != 200 || total <= 0 || (size_t) total > web->size) {
        snprintf(err, err_len, "HTTP %d, %d bytes", status, total);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_vfs_spiffs_unregister("web");
    result = esp_partition_erase_range(web, 0, web->size);

    int offset = 0;
    while (result == ESP_OK && offset < total) {
        const int got = esp_http_client_read(client, s_buf, CHUNK);
        if (got <= 0) {
            snprintf(err, err_len, "transfer broke off at %d of %d", offset, total);
            result = ESP_FAIL;
            break;
        }
        result = esp_partition_write(web, offset, s_buf, got);
        offset += got;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (result != ESP_OK && err[0] == 0) {
        snprintf(err, err_len, "%s", esp_err_to_name(result));
    }

    return result;
}

static esp_err_t update_from_url_post(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    char body[512];
    if (req->content_len >= (int) sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        const int got = httpd_req_recv(req, body + received, req->content_len - received);
        if (got <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Truncated body");
            return ESP_FAIL;
        }
        received += got;
    }
    body[received] = 0;

    cJSON *json = cJSON_Parse(body);
    const cJSON *url = json ? cJSON_GetObjectItemCaseSensitive(json, "url") : NULL;
    const cJSON *target = json ? cJSON_GetObjectItemCaseSensitive(json, "target") : NULL;

    if (!cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) != 0) {
        // Plain http would put an unauthenticated image on the flash of a device that controls a
        // battery. Not worth the convenience.
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "An https:// url is required");
        return ESP_FAIL;
    }

    const bool want_web = cJSON_IsString(target) && strcmp(target->valuestring, "web") == 0;
    char url_copy[320];
    strlcpy(url_copy, url->valuestring, sizeof(url_copy));
    cJSON_Delete(json);

    ESP_LOGI(TAG, "Fetching %s image from %s", want_web ? "web" : "firmware", url_copy);
    ble_central_disconnect();

    char err[96] = { 0 };
    const esp_err_t result = want_web ? fetch_web(url_copy, err, sizeof(err))
                                      : fetch_firmware(url_copy, err, sizeof(err));

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Update from url failed: %s", err);
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "application/json");

        char response[160];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", err);
        httpd_resp_sendstr(req, response);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");

    const esp_timer_create_args_t args = { .callback = &reboot_soon, .name = "url_reboot" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 500 * 1000);
    }

    return ESP_OK;
}

static esp_err_t diag_get(httpd_req_t *req)
{
    if (!auth_guard(req)) {
        return ESP_OK;
    }

    char body[512];
    ws_bridge_stats_json(body, sizeof(body));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

esp_err_t ota_update_register_handlers(httpd_handle_t server)
{
    const httpd_uri_t update = {
        .uri = "/api/update",
        .method = HTTP_POST,
        .handler = update_post,
    };
    esp_err_t err = httpd_register_uri_handler(server, &update);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t update_web = {
        .uri = "/api/update/web",
        .method = HTTP_POST,
        .handler = update_web_post,
    };
    err = httpd_register_uri_handler(server, &update_web);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t from_url = {
        .uri = "/api/update/url",
        .method = HTTP_POST,
        .handler = update_from_url_post,
    };
    err = httpd_register_uri_handler(server, &from_url);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t diag = {
        .uri = "/api/diag",
        .method = HTTP_GET,
        .handler = diag_get,
    };
    err = httpd_register_uri_handler(server, &diag);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t version = {
        .uri = "/api/version",
        .method = HTTP_GET,
        .handler = version_get,
    };
    return httpd_register_uri_handler(server, &version);
}

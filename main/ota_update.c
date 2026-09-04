#include "ota_update.h"

#include "auth.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
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

    const httpd_uri_t version = {
        .uri = "/api/version",
        .method = HTTP_GET,
        .handler = version_get,
    };
    return httpd_register_uri_handler(server, &version);
}

#include "web_server.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "auth.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ota_update.h"
#include "storage.h"
#include "wifi.h"
#include "wifi_setup.h"
#include <unistd.h>

#include "ws_bridge.h"

static const char *TAG = "web";

// Serving a 640 KB bundle a chunk at a time; big enough that the transfer is not dominated by
// per-chunk overhead, small enough not to matter next to the BLE and WiFi stacks.
#define SEND_CHUNK 4096

static char s_chunk[SEND_CHUNK];

static const char *content_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }

    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0)   return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png") == 0)  return "image/png";
    if (strcmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0)  return "image/x-icon";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";

    return "application/octet-stream";
}

/**
 * Tells the browser what this thing is, and how far along setting it up we are.
 *
 * The web app asks its own origin this on startup. Getting JSON with the right marker back is
 * what makes it switch from Web Bluetooth to relaying through us; anything else - a 404, an HTML
 * error page, a timeout - and it stays in direct mode.
 */
static esp_err_t api_bridge_get(httpd_req_t *req)
{
    char body[128];
    snprintf(body, sizeof(body),
             "{\"bridge\":\"marstek-ble-control\",\"version\":1,"
             "\"claimed\":%s,\"authenticated\":%s}",
             auth_is_claimed() ? "true" : "false",
             auth_request_is_authenticated(req) ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    return httpd_resp_sendstr(req, body);
}

static esp_err_t static_file_get(httpd_req_t *req)
{
    // Mount point plus the longest URI the bundle produces, with room to spare.
    char path[192];

    const char *uri = req->uri;

    // Strip a query string; the app does not use one, but a bookmark might carry it.
    size_t uri_len = strcspn(uri, "?#");

    // Anything containing ".." is refused outright rather than normalised. There is nothing above
    // the web root worth reaching, so the safe answer is simply no.
    if (memchr(uri, '.', uri_len) && strstr(uri, "..") != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
        return ESP_FAIL;
    }

    if (uri_len == 1 && uri[0] == '/') {
        // A bridge with no network to join has nothing the app can usefully do, and the app has
        // no way to say so. Send it to the page that can.
        if (!wifi_has_credentials()) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/setup");
            return httpd_resp_send(req, NULL, 0);
        }

        snprintf(path, sizeof(path), "%s/index.html", STORAGE_WEB_ROOT);
    } else {
        snprintf(path, sizeof(path), "%s%.*s", STORAGE_WEB_ROOT, (int) uri_len, uri);
    }

    // Everything in the web partition is stored gzipped, because two OTA slots leave little room
    // for a megabyte of uncompressed bundle. The browser unpacks it; the ESP32 only has to hand
    // over the bytes, which is also a good deal quicker over WiFi.
    bool gzipped = false;
    struct stat info;

    if (stat(path, &info) != 0) {
        char packed[sizeof(path) + 4];
        snprintf(packed, sizeof(packed), "%s.gz", path);

        if (stat(packed, &info) != 0) {
            ESP_LOGW(TAG, "Not found: %s", path);
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }

        gzipped = true;
        // Content type still comes from the name without .gz - the encoding is separate from what
        // the file actually is.
        strlcpy(path, packed, sizeof(path));
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot read file");
        return ESP_FAIL;
    }

    if (gzipped) {
        path[strlen(path) - 3] = '\0';   /* drop ".gz" before deciding the type */
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    httpd_resp_set_type(req, content_type_for(path));

    // The bundle filename carries a content hash, so it can be cached hard. index.html must not
    // be, or a firmware update would leave browsers on the previous bundle forever.
    const bool hashed_asset = strstr(path, "/assets/") != NULL;
    httpd_resp_set_hdr(req, "Cache-Control", hashed_asset ? "public, max-age=31536000, immutable" : "no-cache");

    size_t read;
    do {
        read = fread(s_chunk, 1, sizeof(s_chunk), file);
        if (read > 0 && httpd_resp_send_chunk(req, s_chunk, read) != ESP_OK) {
            fclose(file);
            ESP_LOGE(TAG, "Send failed for %s", path);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    } while (read == sizeof(s_chunk));

    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

/** Taking this over means closing the socket ourselves; the server no longer does it. */
static void on_session_close(httpd_handle_t handle, int fd)
{
    (void) handle;

    ws_bridge_session_closed(fd);
    close(fd);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // The only reliable notice that a browser has gone. A page reload or a closed tab drops the
    // connection without sending a WebSocket close frame, so the frame handler never hears about
    // it and the relay would keep counting a client that left minutes ago.
    config.close_fn = on_session_close;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    // Default is 8, and the api routes alone are well past that now.
    config.max_uri_handlers = 24;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t api_bridge = {
        .uri = "/api/bridge",
        .method = HTTP_GET,
        .handler = api_bridge_get,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api_bridge));

    ESP_ERROR_CHECK(auth_register_handlers(server));
    ESP_ERROR_CHECK(wifi_setup_register_handlers(server));
    ESP_ERROR_CHECK(ota_update_register_handlers(server));
    ESP_ERROR_CHECK(ws_bridge_start(server));

    // Registered last so the API routes above win; the wildcard is the fallback.
    const httpd_uri_t static_files = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_get,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &static_files));

    ESP_LOGI(TAG, "HTTP server listening on port %d", config.server_port);

    return ESP_OK;
}

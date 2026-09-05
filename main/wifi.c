#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "status_led.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASSWORD "wifi_pass"

#define NVS_KEY_HOSTNAME "hostname"

#define DEFAULT_HOSTNAME "marstek-bridge"

// Back off after a burst of quick retries. A bridge sits next to a battery in a cupboard with
// nobody watching, so it must keep trying indefinitely rather than give up - but hammering the
// access point every few hundred milliseconds forever is antisocial and drains nothing useful.
#define WIFI_FAST_RETRIES 5
#define WIFI_RETRY_DELAY_US (10 * 1000 * 1000)

// Long enough for a slow DHCP server, short enough that someone watching a browser tab does not
// conclude the board is dead.
#define WIFI_JOIN_TIMEOUT_MS 20000

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1

static bool s_connected = false;
static int s_retries = 0;
static bool s_ap_active = false;

static char s_ssid[WIFI_SSID_MAX];
static char s_password[WIFI_PASSWORD_MAX];
static char s_ip[16];
static char s_hostname[WIFI_HOSTNAME_MAX];

// Whether the credentials in use came from NVS rather than the build. Anything compiled in is
// copied to NVS on the first successful join, so a later firmware that no longer carries them -
// every published build - still finds its way onto the network.
static bool s_from_nvs = false;

// Suppresses the automatic retry while a candidate set of credentials is being tried, so a
// failure reports back instead of quietly turning into a reconnect loop.
static volatile bool s_trying = false;

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_fallback_timer;

/**
 * Answer to marstek-bridge.local, so the bridge can be found without hunting through a router.
 *
 * Worth more here than on most devices: the address is what someone types after an update, and
 * what a browser is left holding when DHCP hands out a different one. Failing is not fatal - the
 * IP still works - so nothing here stops the boot.
 */
static bool s_announced = false;

static void announce_ourselves(void)
{
    if (!s_announced) {
        if (mdns_init() != ESP_OK) {
            ESP_LOGW(TAG, "No mDNS - reachable by address only");
            return;
        }

        mdns_instance_name_set("Marstek BLE Bridge");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_announced = true;
    }

    mdns_hostname_set(s_hostname);

    ESP_LOGI(TAG, "Also reachable as %s.local", s_hostname);
}

/**
 * A name that can safely be a DNS label and a WiFi hostname.
 *
 * Deliberately narrower than the standard allows: lower case only, so nobody has to remember the
 * capitalisation of something they will type into an address bar, and no leading or trailing
 * hyphen, which some resolvers refuse outright.
 */
static bool hostname_is_valid(const char *name)
{
    const size_t len = name ? strlen(name) : 0;

    if (len == 0 || len >= WIFI_HOSTNAME_MAX) {
        return false;
    }
    if (name[0] == '-' || name[len - 1] == '-') {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        const char c = name[i];
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!allowed) {
            return false;
        }
    }

    return true;
}

static void hostname_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_hostname);
        if (nvs_get_str(nvs, NVS_KEY_HOSTNAME, s_hostname, &len) != ESP_OK) {
            s_hostname[0] = '\0';
        }
        nvs_close(nvs);
    }

    if (!hostname_is_valid(s_hostname)) {
        strlcpy(s_hostname, DEFAULT_HOSTNAME, sizeof(s_hostname));
    }
}

const char *wifi_hostname(void)
{
    return s_hostname;
}

esp_err_t wifi_set_hostname(const char *name)
{
    if (!hostname_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, NVS_KEY_HOSTNAME, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_hostname, name, sizeof(s_hostname));

    // mDNS takes the new name straight away. What the router lists does not: that name went out
    // with the DHCP request and is only re-sent on the next lease.
    if (s_announced) {
        mdns_hostname_set(s_hostname);
    }

    ESP_LOGI(TAG, "Renamed to %s.local", s_hostname);
    return ESP_OK;
}

static void credentials_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_len = sizeof(s_ssid);
        size_t password_len = sizeof(s_password);

        if (nvs_get_str(nvs, NVS_KEY_SSID, s_ssid, &ssid_len) == ESP_OK && s_ssid[0]) {
            s_from_nvs = true;
            if (nvs_get_str(nvs, NVS_KEY_PASSWORD, s_password, &password_len) != ESP_OK) {
                s_password[0] = '\0';
            }
        }

        nvs_close(nvs);
    }

    if (!s_from_nvs) {
        strlcpy(s_ssid, CONFIG_BRIDGE_WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_password, CONFIG_BRIDGE_WIFI_PASSWORD, sizeof(s_password));
    }
}

static esp_err_t credentials_save(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_PASSWORD, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return err;
}

static void apply_station_config(const char *ssid, const char *password)
{
    wifi_config_t config = { 0 };
    strlcpy((char *) config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *) config.sta.password, password ? password : "", sizeof(config.sta.password));

    esp_wifi_set_config(WIFI_IF_STA, &config);
}

/**
 * Open an access point of our own after failing to join the configured network.
 *
 * A bridge sits wherever the storage sits, usually with no console attached, so a failure to join
 * otherwise leaves it mute with no way to ask what went wrong. The station side keeps retrying in
 * the background, so a network that comes back is picked up without intervention.
 */
static void start_fallback_ap(void *arg)
{
    (void) arg;

    if (s_connected || s_ap_active) {
        return;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        return;
    }

    wifi_config_t ap = { 0 };
    int len = snprintf((char *) ap.ap.ssid, sizeof(ap.ap.ssid),
                       "Marstek-Bridge-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = len;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;

    strlcpy((char *) ap.ap.password, CONFIG_BRIDGE_FALLBACK_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.authmode = strlen(CONFIG_BRIDGE_FALLBACK_AP_PASSWORD) >= 8
        ? WIFI_AUTH_WPA2_PSK
        : WIFI_AUTH_OPEN;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "Could not bring up the fallback access point");
        return;
    }

    s_ap_active = true;
    status_led_set(STATUS_LED_AP);

    // The setup page is reached over this access point, and a name is easier to pass on than the
    // gateway address someone would otherwise have to guess.
    announce_ourselves();

    if (s_ssid[0]) {
        ESP_LOGW(TAG, "No WiFi - serving \"%s\" instead, still retrying \"%s\" in the background",
                 (char *) ap.ap.ssid, s_ssid);
    } else {
        ESP_LOGW(TAG, "No network configured - serving \"%s\" for setup", (char *) ap.ap.ssid);
    }
}

static void retry_now(void *arg)
{
    (void) arg;
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) data;

    if (base != WIFI_EVENT) {
        return;
    }

    switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_ssid[0]) {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            s_ip[0] = '\0';

            if (s_trying) {
                xEventGroupSetBits(s_events, BIT_FAILED);
                break;
            }

            if (!s_ap_active) {
                status_led_set(STATUS_LED_NO_WIFI);
            }

            if (s_retries < WIFI_FAST_RETRIES) {
                s_retries++;
                ESP_LOGW(TAG, "Disconnected, retry %d", s_retries);
                esp_wifi_connect();
            } else {
                // Retry from a timer rather than sleeping here: this runs on the event loop task,
                // and blocking it would stall every other event in the system.
                ESP_LOGW(TAG, "Disconnected, backing off");
                esp_timer_stop(s_retry_timer);
                ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, WIFI_RETRY_DELAY_US));
            }
            break;

        default:
            break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;

    s_connected = true;
    s_retries = 0;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));

    esp_timer_stop(s_fallback_timer);
    status_led_set(STATUS_LED_ONLINE);
    xEventGroupSetBits(s_events, BIT_CONNECTED);

    announce_ourselves();

    if (!s_from_nvs && s_ssid[0]) {
        // Credentials that only exist in the build would be lost the moment a published firmware
        // is installed over this one. Writing them down now keeps that update from stranding a
        // board that was working a minute earlier.
        if (credentials_save(s_ssid, s_password) == ESP_OK) {
            s_from_nvs = true;
            ESP_LOGI(TAG, "Stored the compiled-in credentials so an update keeps them");
        }
    }

    ESP_LOGI(TAG, "Connected, address %s", s_ip);
}

bool wifi_is_connected(void)
{
    return s_connected;
}

bool wifi_fallback_ap_active(void)
{
    return s_ap_active;
}

bool wifi_has_credentials(void)
{
    return s_ssid[0] != '\0';
}

const char *wifi_ssid(void)
{
    return s_ssid;
}

const char *wifi_ip(void)
{
    return s_ip;
}

int8_t wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

size_t wifi_scan(wifi_scan_entry_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }

    const wifi_scan_config_t scan = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        return 0;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }

    // Bounded rather than sized to the result: this runs on a chip with a Bluetooth stack and an
    // HTTP server already resident, and nobody picks their network from the 60th entry.
    if (found > 32) {
        found = 32;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(*records));
    if (!records) {
        esp_wifi_clear_ap_list();
        return 0;
    }

    esp_wifi_scan_get_ap_records(&found, records);

    // Already strongest first, so the first sighting of a name is the one worth keeping - the
    // rest are the same network seen through another access point or band.
    size_t written = 0;
    for (uint16_t i = 0; i < found && written < max; i++) {
        const char *ssid = (const char *) records[i].ssid;
        if (!ssid[0]) {
            continue;
        }

        bool seen = false;
        for (size_t j = 0; j < written; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        strlcpy(out[written].ssid, ssid, sizeof(out[written].ssid));
        out[written].rssi = records[i].rssi;
        out[written].secured = records[i].authmode != WIFI_AUTH_OPEN;
        written++;
    }

    free(records);
    return written;
}

esp_err_t wifi_provision(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) >= WIFI_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Trying \"%s\"", ssid);

    s_trying = true;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();

    // Let the disconnect this just caused pass through the event loop, so the bits cleared below
    // are not immediately set again by the teardown of the previous association.
    vTaskDelay(pdMS_TO_TICKS(300));

    apply_station_config(ssid, password);
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);

    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(WIFI_JOIN_TIMEOUT_MS));

        err = (bits & BIT_CONNECTED) ? ESP_OK : ESP_ERR_WIFI_NOT_CONNECT;
    }

    s_trying = false;

    if (err == ESP_OK) {
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        strlcpy(s_password, password ? password : "", sizeof(s_password));
        s_from_nvs = true;

        const esp_err_t saved = credentials_save(s_ssid, s_password);
        if (saved != ESP_OK) {
            ESP_LOGE(TAG, "Joined but could not store the credentials: %s", esp_err_to_name(saved));
        }

        ESP_LOGI(TAG, "Provisioned for \"%s\"", s_ssid);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Could not join \"%s\", keeping the previous settings", ssid);

    apply_station_config(s_ssid, s_password);
    s_retries = 0;
    if (s_ssid[0]) {
        esp_wifi_connect();
    }

    return err;
}

esp_err_t wifi_forget(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASSWORD);
    err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGW(TAG, "Credentials forgotten - the next boot opens the setup access point");
    return err;
}

esp_err_t wifi_start(void)
{
    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    hostname_load();
    credentials_load();

    const esp_timer_create_args_t retry_args = {
        .callback = &retry_now,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    const esp_timer_create_args_t fallback_args = {
        .callback = &start_fallback_ap,
        .name = "wifi_fallback",
    };
    ESP_ERROR_CHECK(esp_timer_create(&fallback_args, &s_fallback_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *station = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // What the router shows in its client list. Separate from mDNS, and set before the interface
    // comes up because the name goes out with the DHCP request.
    esp_netif_set_hostname(station, s_hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL, NULL));

    // Station mode either way: an unconfigured bridge still needs it to scan for the networks it
    // is about to be pointed at.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_station_config(s_ssid, s_password);

    // The bridge holds a BLE connection and a WebSocket for hours on end, so modem sleep is not
    // what it eventually wants - it adds latency to every relayed frame for a power saving that
    // is irrelevant on a mains-powered device. During bring-up it is on by default, because the
    // extra draw is one more thing that can tip a marginal supply over.
#if CONFIG_BRIDGE_WIFI_POWER_SAVE
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    // Only valid once the driver is running. Transmit peaks scale with this, and a board whose
    // supply cannot follow them resets the instant the radio comes up - before anything useful
    // reaches the console.
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_BRIDGE_WIFI_MAX_TX_POWER));

    int8_t actual = 0;
    if (esp_wifi_get_max_tx_power(&actual) == ESP_OK) {
        ESP_LOGI(TAG, "Transmit power capped at %d (%.2f dBm)", actual, actual / 4.0);
    }

    if (s_ssid[0]) {
        ESP_LOGI(TAG, "Joining \"%s\"", s_ssid);
        ESP_ERROR_CHECK(esp_timer_start_once(
            s_fallback_timer, (uint64_t) CONFIG_BRIDGE_FALLBACK_AP_SECONDS * 1000000));
    } else {
        // Nothing to wait for. A board fresh off the flasher is only reachable through its own
        // access point, so it goes up now rather than after a timeout spent joining nothing.
        ESP_LOGW(TAG, "No credentials stored - opening the setup access point");
        start_fallback_ap(NULL);
    }

    return ESP_OK;
}

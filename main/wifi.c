#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "status_led.h"

static const char *TAG = "wifi";

static bool s_connected = false;
static int s_retries = 0;

// Back off after a burst of quick retries. A bridge sits next to a battery in a cupboard with
// nobody watching, so it must keep trying indefinitely rather than give up - but hammering the
// access point every few hundred milliseconds forever is antisocial and drains nothing useful.
#define WIFI_FAST_RETRIES 5
#define WIFI_RETRY_DELAY_US (10 * 1000 * 1000)

static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_fallback_timer;
static bool s_ap_active = false;

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

    ESP_LOGW(TAG, "No WiFi - serving \"%s\" instead, still retrying \"%s\" in the background",
             (char *) ap.ap.ssid, CONFIG_BRIDGE_WIFI_SSID);
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
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
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

    esp_timer_stop(s_fallback_timer);
    status_led_set(STATUS_LED_ONLINE);

    ESP_LOGI(TAG, "Connected, address " IPSTR, IP2STR(&event->ip_info.ip));
}

bool wifi_is_connected(void)
{
    return s_connected;
}

bool wifi_fallback_ap_active(void)
{
    return s_ap_active;
}

int8_t wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

esp_err_t wifi_start(void)
{
    if (strlen(CONFIG_BRIDGE_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "No SSID configured - set one via idf.py menuconfig for now");
        return ESP_ERR_INVALID_STATE;
    }

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
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL, NULL));

    wifi_config_t config = { 0 };
    strlcpy((char *) config.sta.ssid, CONFIG_BRIDGE_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *) config.sta.password, CONFIG_BRIDGE_WIFI_PASSWORD, sizeof(config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));

    // The bridge holds a BLE connection and a WebSocket for hours on end, so modem sleep is not
    // what it eventually wants - it adds latency to every relayed frame for a power saving that
    // is irrelevant on a mains-powered device. During bring-up it is on by default, because the
    // extra draw is one more thing that can tip a marginal supply over.
#if CONFIG_BRIDGE_WIFI_POWER_SAVE
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif

    ESP_LOGI(TAG, "Joining \"%s\"", CONFIG_BRIDGE_WIFI_SSID);

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_timer_start_once(
        s_fallback_timer, (uint64_t) CONFIG_BRIDGE_FALLBACK_AP_SECONDS * 1000000));

    // Only valid once the driver is running. Transmit peaks scale with this, and a board whose
    // supply cannot follow them resets the instant the radio comes up - before anything useful
    // reaches the console.
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_BRIDGE_WIFI_MAX_TX_POWER));

    int8_t actual = 0;
    if (esp_wifi_get_max_tx_power(&actual) == ESP_OK) {
        ESP_LOGI(TAG, "Transmit power capped at %d (%.2f dBm)", actual, actual / 4.0);
    }

    return ESP_OK;
}

#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

static bool s_connected = false;
static int s_retries = 0;

// Back off after a burst of quick retries. A bridge sits next to a battery in a cupboard with
// nobody watching, so it must keep trying indefinitely rather than give up - but hammering the
// access point every few hundred milliseconds forever is antisocial and drains nothing useful.
#define WIFI_FAST_RETRIES 5
#define WIFI_RETRY_DELAY_US (10 * 1000 * 1000)

static esp_timer_handle_t s_retry_timer;

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

    ESP_LOGI(TAG, "Connected, address " IPSTR, IP2STR(&event->ip_info.ip));
}

bool wifi_is_connected(void)
{
    return s_connected;
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

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

    // The bridge holds a BLE connection and a WebSocket for hours on end; letting WiFi sleep
    // between beacons adds latency to every relayed frame for a power saving that is irrelevant
    // on a mains-powered device.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Joining \"%s\"", CONFIG_BRIDGE_WIFI_SSID);
    return esp_wifi_start();
}

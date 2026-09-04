#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "auth.h"
#include "boot_guard.h"
#include "status_led.h"
#include "storage.h"
#include "web_server.h"
#include "wifi.h"

static const char *TAG = "bridge";

void app_main(void)
{
    ESP_LOGI(TAG, "Marstek BLE Bridge starting");

    // Before anything that can fail, so the blink itself says "the firmware is running". Silence
    // then means the board never got this far, which is a different problem entirely.
    status_led_init();

    ESP_ERROR_CHECK(storage_init());

    // Before anything else that could fail: if the previous boot never became reachable, this
    // goes back to the firmware that did.
    ESP_ERROR_CHECK(boot_guard_begin());

    ESP_ERROR_CHECK(auth_init());

    if (wifi_start() != ESP_OK) {
        // Without a network the bridge is useless, but it must not reboot in a loop either: a
        // console message that stays readable is more useful than a crash dump scrolling past.
        ESP_LOGE(TAG, "WiFi could not be started - configure credentials and reflash");
        return;
    }

    // The HTTP server is started regardless of link state; it binds to the interface and starts
    // answering as soon as an address arrives.
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "Waiting for network");

    // Either route counts as reachable: the fallback access point is a perfectly good way to get
    // at the bridge, and a router that happens to be down should not send it back a version.
    while (!wifi_is_connected() && !wifi_fallback_ap_active()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    boot_guard_mark_healthy();
    ESP_LOGI(TAG, "Ready - serving the web app");
}

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "storage.h"
#include "web_server.h"
#include "wifi.h"

static const char *TAG = "bridge";

void app_main(void)
{
    ESP_LOGI(TAG, "Marstek BLE Bridge starting");

    ESP_ERROR_CHECK(storage_init());

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
    while (!wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Ready - serving the web app");
}

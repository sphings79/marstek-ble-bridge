#include "boot_guard.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "guard";

#define NVS_NAMESPACE "bridge"
#define NVS_KEY_PENDING "boot_pending"

static bool s_healthy = false;

static void set_pending(uint8_t value)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    if (value) {
        nvs_set_u8(nvs, NVS_KEY_PENDING, value);
    } else {
        nvs_erase_key(nvs, NVS_KEY_PENDING);
    }

    nvs_commit(nvs);
    nvs_close(nvs);
}

static uint8_t get_pending(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return 0;
    }

    uint8_t value = 0;
    if (nvs_get_u8(nvs, NVS_KEY_PENDING, &value) != ESP_OK) {
        value = 0;
    }

    nvs_close(nvs);
    return value;
}

esp_err_t boot_guard_begin(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);

    if (get_pending()) {
        // The previous boot set this and never cleared it, so it never became reachable.
        set_pending(0);

        if (other && other != running) {
            ESP_LOGE(TAG, "Previous boot never became reachable - going back to %s", other->label);
            if (esp_ota_set_boot_partition(other) == ESP_OK) {
                esp_restart();
            }
            ESP_LOGE(TAG, "Could not switch slots; carrying on with this one");
        } else {
            ESP_LOGW(TAG, "Previous boot never became reachable, but there is no other slot");
        }
    }

    set_pending(1);
    ESP_LOGI(TAG, "Running from %s, watching for reachability",
             running ? running->label : "?");

    return ESP_OK;
}

void boot_guard_mark_healthy(void)
{
    if (s_healthy) {
        return;
    }

    s_healthy = true;
    set_pending(0);
    ESP_LOGI(TAG, "Reachable - keeping this firmware");
}

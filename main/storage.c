#include "storage.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";

static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // A firmware update can leave the old NVS layout behind. Losing the settings is
        // recoverable - the bridge falls back to unclaimed and unbound - while refusing to boot
        // is not.
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t web_partition_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_WEB_ROOT,
        .partition_label = "web",
        .max_files = 8,
        // The web partition is written by the flasher, never by us. Reformatting on a bad mount
        // would silently throw the app away and leave a bridge that serves nothing, which is
        // harder to diagnose than a clear error.
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot mount web partition: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "Web partition mounted: %u of %u bytes used", (unsigned) used, (unsigned) total);
    }

    return ESP_OK;
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    return web_partition_mount();
}

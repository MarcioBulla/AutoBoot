#include "input_system/storage_vfs.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "storage_vfs";

esp_err_t storage_vfs_init(void)
{
    static bool mounted = false;

    if (mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS info unavailable: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    }

    mounted = true;
    return ESP_OK;
}

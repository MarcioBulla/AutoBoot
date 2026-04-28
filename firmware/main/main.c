#include "esp_log.h"

#include "connections/wifi.h"

static const char *TAG = "main";

void app_main(void)
{
    if (wifi_start_console_task() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi console task failed to start");
    }
}

#include "esp_log.h"

#include "control/control.h"

static const char *TAG = "main";

void app_main(void)
{
    if (control_start_uart_task() != ESP_OK) {
        ESP_LOGE(TAG, "UART control task failed to start");
    }
}

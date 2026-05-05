#include "wifi/wifi.h"
#include "wifi/wifi_internal.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "wifi_boot";

static TaskHandle_t s_wifi_boot_connect_task_handle = NULL;
static volatile bool s_wifi_boot_connect_cancel_requested = false;

bool wifi_boot_connect_should_cancel(void)
{
    return s_wifi_boot_connect_cancel_requested;
}

static esp_err_t wifi_connect_saved_credentials_on_boot(void)
{
    char ssid[33];
    char password[65];

    wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));

    esp_err_t ret = wifi_load_credentials_from_nvs(ssid, sizeof(ssid), password, sizeof(password));
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read saved Wi-Fi credentials on boot: %s", esp_err_to_name(ret));
        wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
        return ESP_OK;
    }

    int attempt = 1;
  
    while (true) {
    
        ESP_LOGI(TAG,
                 "Auto-connecting to saved Wi-Fi SSID '%s' (attempt %d)",
                 ssid,
                 attempt);
        attempt++;

        if (wifi_boot_connect_should_cancel()) {
            break;
        }

        ret = wifi_connect_with_credentials(ssid,
                                            password,
                                            pdMS_TO_TICKS(CONFIG_AUTOBOOT_WIFI_BOOT_CONNECT_TIMEOUT_MS),
                                            true);
        if (ret == ESP_OK) {
            break;
        }

        if (ret == ESP_ERR_INVALID_STATE && wifi_boot_connect_should_cancel()) {
            break;
        }

        ESP_LOGW(TAG, "Automatic Wi-Fi connection attempt failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(CONFIG_AUTOBOOT_WIFI_BOOT_CONNECT_RETRY_DELAY_MS));
    }

    wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
    return ESP_OK;
}

static void wifi_boot_connect_task(void *arg)
{
    esp_err_t ret = wifi_connect_saved_credentials_on_boot();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Boot Wi-Fi connection task failed: %s", esp_err_to_name(ret));
    }

    s_wifi_boot_connect_task_handle = NULL;
    vTaskDelete(NULL);
}

void wifi_cancel_boot_auto_connect(void)
{
    s_wifi_boot_connect_cancel_requested = true;
}

esp_err_t wifi_wait_boot_auto_connect_stopped(uint32_t timeout_ms)
{
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (s_wifi_boot_connect_task_handle != NULL) {
        if ((xTaskGetTickCount() - start_ticks) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return ESP_OK;
}

esp_err_t wifi_start_boot_auto_connect(void)
{
    if (s_wifi_boot_connect_task_handle == NULL) {
        s_wifi_boot_connect_cancel_requested = false;
        BaseType_t task_ok = xTaskCreate(wifi_boot_connect_task,
                                         "wifi_boot_connect",
                                         CONFIG_AUTOBOOT_WIFI_BOOT_TASK_STACK_SIZE,
                                         NULL,
                                         4,
                                         &s_wifi_boot_connect_task_handle);
        if (task_ok != pdPASS) {
            s_wifi_boot_connect_task_handle = NULL;
            ESP_LOGW(TAG, "Failed to start automatic Wi-Fi connection task");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

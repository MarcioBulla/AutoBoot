#include "wifi/wifi.h"
#include "wifi/wifi_internal.h"

#include <string.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_wifi_stack_ready = false;
static wifi_err_reason_t s_last_disconnect_reason = WIFI_REASON_UNSPECIFIED;
static bool s_wifi_has_ip = false;
static bool s_disconnect_expected = false;

void wifi_clear_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    memset(ssid, 0, ssid_size);
    memset(password, 0, password_size);
}

static esp_err_t wifi_status_led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(CONFIG_AUTOBOOT_WIFI_STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = CONFIG_AUTOBOOT_WIFI_STATUS_LED_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    return gpio_set_level((gpio_num_t)CONFIG_AUTOBOOT_WIFI_STATUS_LED_GPIO,
                          CONFIG_AUTOBOOT_WIFI_STATUS_LED_ACTIVE_LOW ? 1 : 0);
}

void wifi_status_led_set(bool enabled)
{
    int level = enabled ? 1 : 0;
    if (CONFIG_AUTOBOOT_WIFI_STATUS_LED_ACTIVE_LOW) {
        level = !level;
    }

    esp_err_t ret = gpio_set_level((gpio_num_t)CONFIG_AUTOBOOT_WIFI_STATUS_LED_GPIO, level);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update Wi-Fi status LED: %s", esp_err_to_name(ret));
    }
}

void wifi_set_disconnect_expected(bool expected)
{
    s_disconnect_expected = expected;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        bool restart_required = s_wifi_has_ip && !s_disconnect_expected;
        s_last_disconnect_reason = disconnected->reason;
        s_wifi_has_ip = false;

        ESP_LOGW(TAG, "Disconnected from AP, reason=%d", disconnected->reason);
        wifi_status_led_set(false);
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        if (restart_required) {
            ESP_LOGW(TAG,
                     "Wi-Fi connection lost after IP was acquired. Restarting ESP32, reason=%d",
                     disconnected->reason);
            vTaskDelay(pdMS_TO_TICKS(250));
            esp_restart();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_last_disconnect_reason = WIFI_REASON_UNSPECIFIED;
        s_wifi_has_ip = true;
        s_disconnect_expected = false;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_status_led_set(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_stack_init(void)
{
    if (s_wifi_stack_ready) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = wifi_status_led_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    s_wifi_stack_ready = true;
    return ESP_OK;
}

esp_err_t wifi_init(void)
{
    esp_err_t ret = wifi_stack_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi stack initialization failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t wifi_connect_with_credentials(const char *ssid,
                                        const char *password,
                                        TickType_t timeout_ticks,
                                        bool cancel_on_console_request)
{
    esp_err_t ret;

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    s_last_disconnect_reason = WIFI_REASON_UNSPECIFIED;
    s_wifi_has_ip = false;
    s_disconnect_expected = true;
    wifi_status_led_set(false);

    ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT && ret != ESP_ERR_WIFI_NOT_STARTED) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(250));

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    if (password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Connecting to SSID '%s'", ssid);

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(350));

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        return ret;
    }

    TickType_t start_ticks = xTaskGetTickCount();
    EventBits_t bits = 0;

    while (true) {
        if (cancel_on_console_request && wifi_boot_connect_should_cancel()) {
            ESP_LOGI(TAG, "Automatic Wi-Fi connection canceled by console input");
            wifi_set_disconnect_expected(true);
            esp_err_t disconnect_ret = esp_wifi_disconnect();
            if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "Failed to cancel Wi-Fi connection: %s", esp_err_to_name(disconnect_ret));
            }
            wifi_status_led_set(false);
            return ESP_ERR_INVALID_STATE;
        }

        TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
        if (elapsed_ticks >= timeout_ticks) {
            break;
        }

        TickType_t wait_ticks = timeout_ticks - elapsed_ticks;
        if (wait_ticks > pdMS_TO_TICKS(200)) {
            wait_ticks = pdMS_TO_TICKS(200);
        }

        bits = xEventGroupWaitBits(s_wifi_event_group,
                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                   pdFALSE,
                                   pdFALSE,
                                   wait_ticks);

        if (bits & (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)) {
            break;
        }
    }

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected successfully");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi connection timed out or failed (reason=%d)", s_last_disconnect_reason);
    return ESP_FAIL;
}

void wifi_print_status(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    if (ret == ESP_OK) {
        printf("Wi-Fi connected to '%s' | RSSI %d\n", (const char *)ap_info.ssid, ap_info.rssi);
        return;
    }

    printf("Wi-Fi disconnected\n");
}

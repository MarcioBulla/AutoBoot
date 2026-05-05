#include "control/control.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "input_system/input_test.h"
#include "wifi/wifi.h"

static const char *TAG = "control";

static TaskHandle_t s_control_uart_task_handle = NULL;

static void control_print_wifi_menu(void)
{
    printf("\nMenu Wi-Fi\n");
    printf("1. Conectar/configurar Wi-Fi\n");
    printf("2. Limpar credenciais salvas\n");
    printf("3. Mostrar status\n");
    printf("0. Voltar\n");
}

static void control_take_uart_control(void)
{
    wifi_cancel_boot_auto_connect();

    esp_err_t ret = wifi_wait_boot_auto_connect_stopped(CONFIG_AUTOBOOT_UART_CONTROL_STOP_WAIT_MS);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Timed out waiting for automatic Wi-Fi connection to stop");
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Automatic Wi-Fi connection stop wait failed: %s", esp_err_to_name(ret));
    }
}

static void control_handle_wifi_menu(void)
{
    char command[16];

    while (true) {
        control_print_wifi_menu();

        int command_len = input_test_prompt_line("Opcao: ", command, sizeof(command), false);
        if (command_len <= 0) {
            ESP_LOGW(TAG, "Invalid menu option");
            continue;
        }

        if (strcmp(command, "1") == 0) {
            control_take_uart_control();
            esp_err_t ret = wifi_connect_from_console();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi connection flow failed: %s", esp_err_to_name(ret));
            }
            continue;
        }

        if (strcmp(command, "2") == 0) {
            control_take_uart_control();
            esp_err_t ret = wifi_erase_saved_credentials();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi credentials erased from NVS");
            } else {
                ESP_LOGE(TAG, "Failed to erase Wi-Fi credentials: %s", esp_err_to_name(ret));
            }
            continue;
        }

        if (strcmp(command, "3") == 0) {
            wifi_print_status();
            continue;
        }

        if (strcmp(command, "0") == 0) {
            return;
        }

        ESP_LOGW(TAG, "Unknown menu option '%s'", command);
    }
}

static void control_uart_task(void *arg)
{
    char trigger[8];

    printf("\nConsole Wi-Fi pronto. Pressione Enter para abrir o menu.\n");

    while (true) {
        int trigger_len = input_test_prompt_line_mode("\n[Enter] menu Wi-Fi > ",
                                                      trigger,
                                                      sizeof(trigger),
                                                      true,
                                                      true);
        if (trigger_len < 0) {
            ESP_LOGE(TAG, "Console input failed");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        control_take_uart_control();
        control_handle_wifi_menu();
    }
}

esp_err_t control_start_uart_task(void)
{
    esp_err_t ret = input_test_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Console initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_control_uart_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t task_ok = xTaskCreate(control_uart_task,
                                     "control_uart",
                                     CONFIG_AUTOBOOT_UART_CONTROL_TASK_STACK_SIZE,
                                     NULL,
                                     5,
                                     &s_control_uart_task_handle);
    if (task_ok != pdPASS) {
        s_control_uart_task_handle = NULL;
        return ESP_FAIL;
    }

    ret = wifi_start_boot_auto_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Automatic Wi-Fi connection task did not start: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

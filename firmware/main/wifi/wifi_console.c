#include "wifi/wifi.h"
#include "wifi/wifi_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "input_system/input_test.h"

static const char *TAG = "wifi_console";

static void wifi_sort_ap_records(wifi_ap_record_t *records, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (records[j].rssi > records[i].rssi) {
                wifi_ap_record_t tmp = records[i];
                records[i] = records[j];
                records[j] = tmp;
            }
        }
    }
}

static size_t wifi_compact_unique_ssids(wifi_ap_record_t *records, size_t count)
{
    size_t unique_count = 0;

    for (size_t i = 0; i < count; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }

        bool already_seen = false;
        for (size_t j = 0; j < unique_count; ++j) {
            if (strcmp((const char *)records[i].ssid, (const char *)records[j].ssid) == 0) {
                already_seen = true;
                break;
            }
        }

        if (!already_seen) {
            if (unique_count != i) {
                records[unique_count] = records[i];
            }
            unique_count++;
        }
    }

    return unique_count;
}

static const char *wifi_auth_mode_to_string(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            return "aberta";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:
            return "WAPI";
        default:
            return "desconhecida";
    }
}

static esp_err_t wifi_scan_access_points(wifi_ap_record_t *records, size_t max_records, size_t *record_count)
{
    uint16_t ap_count = (uint16_t)max_records;
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t ret;

    if (records == NULL || record_count == NULL || max_records == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        return ret;
    }

    wifi_set_disconnect_expected(true);
    ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        wifi_set_disconnect_expected(false);
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        wifi_set_disconnect_expected(false);
        return ret;
    }
    wifi_set_disconnect_expected(false);

    ret = esp_wifi_scan_get_ap_records(&ap_count, records);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_sort_ap_records(records, ap_count);
    *record_count = wifi_compact_unique_ssids(records, ap_count);
    return ESP_OK;
}

static esp_err_t wifi_prompt_manual_ssid(char *ssid, size_t ssid_size)
{
    int ssid_len = input_test_prompt_line("SSID manual: ", ssid, ssid_size, false);
    if (ssid_len <= 0) {
        ESP_LOGE(TAG, "Failed to read SSID");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void wifi_print_scan_page(const wifi_ap_record_t *records, size_t record_count, size_t page_index)
{
    size_t start = page_index * CONFIG_AUTOBOOT_WIFI_SCAN_PAGE_SIZE;
    size_t end = start + CONFIG_AUTOBOOT_WIFI_SCAN_PAGE_SIZE;

    if (end > record_count) {
        end = record_count;
    }

    printf("\nRedes Wi-Fi encontradas (%u total)\n", (unsigned int)record_count);
    printf("Pagina %u\n", (unsigned int)(page_index + 1));
    for (size_t i = start; i < end; ++i) {
        printf("%u. %s | RSSI %d | %s\n",
               (unsigned int)(i - start + 1),
               (const char *)records[i].ssid,
               records[i].rssi,
               wifi_auth_mode_to_string(records[i].authmode));
    }
    printf("n = proximas %d | p = %d anteriores | r = escanear de novo | m = SSID manual\n",
           CONFIG_AUTOBOOT_WIFI_SCAN_PAGE_SIZE,
           CONFIG_AUTOBOOT_WIFI_SCAN_PAGE_SIZE);
}

static esp_err_t wifi_select_scanned_ssid(char *ssid, size_t ssid_size)
{
    wifi_ap_record_t records[CONFIG_AUTOBOOT_WIFI_SCAN_MAX_APS];
    char command[16];
    size_t record_count = 0;
    size_t page_index = 0;
    esp_err_t ret;

    while (true) {
        ret = wifi_scan_access_points(records, CONFIG_AUTOBOOT_WIFI_SCAN_MAX_APS, &record_count);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(ret));
            return ret;
        }

        if (record_count == 0) {
            ESP_LOGW(TAG, "No visible SSID found in scan");
            return wifi_prompt_manual_ssid(ssid, ssid_size);
        }

        page_index = 0;
        while (true) {
            size_t start = page_index * CONFIG_AUTOBOOT_WIFI_SCAN_PAGE_SIZE;
            size_t end = start + CONFIG_AUTOBOOT_WIFI_SCAN_PAGE_SIZE;

            if (end > record_count) {
                end = record_count;
            }

            wifi_print_scan_page(records, record_count, page_index);

            char prompt[40];
            snprintf(prompt,
                     sizeof(prompt),
                     "Escolha [1-%d/n/p/r/m]: ",
                     CONFIG_AUTOBOOT_WIFI_SCAN_PAGE_SIZE);
            int command_len = input_test_prompt_line(prompt, command, sizeof(command), false);
            if (command_len <= 0) {
                ESP_LOGE(TAG, "Failed to read Wi-Fi selection");
                return ESP_FAIL;
            }

            if (strcmp(command, "n") == 0 || strcmp(command, "N") == 0) {
                if (end < record_count) {
                    page_index++;
                } else {
                    ESP_LOGW(TAG, "Already at last page");
                }
                continue;
            }

            if (strcmp(command, "p") == 0 || strcmp(command, "P") == 0) {
                if (page_index > 0) {
                    page_index--;
                } else {
                    ESP_LOGW(TAG, "Already at first page");
                }
                continue;
            }

            if (strcmp(command, "r") == 0 || strcmp(command, "R") == 0) {
                break;
            }

            if (strcmp(command, "m") == 0 || strcmp(command, "M") == 0) {
                return wifi_prompt_manual_ssid(ssid, ssid_size);
            }

            int option = atoi(command);
            if (option < 1 || (size_t)option > (end - start)) {
                ESP_LOGW(TAG, "Invalid option '%s'", command);
                continue;
            }

            strlcpy(ssid, (const char *)records[start + (size_t)option - 1].ssid, ssid_size);
            ESP_LOGI(TAG, "Selected SSID '%s'", ssid);
            return ESP_OK;
        }
    }
}

static esp_err_t wifi_prompt_for_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    esp_err_t ret = wifi_select_scanned_ssid(ssid, ssid_size);
    if (ret != ESP_OK) {
        return ret;
    }

    int password_len = input_test_prompt_line("Senha: ", password, password_size, true);
    if (password_len < 0) {
        ESP_LOGE(TAG, "Failed to read password");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Credentials received for SSID '%s' (%s)", ssid, password_len > 0 ? "WPA/WPA2" : "open");
    return ESP_OK;
}

static esp_err_t wifi_prompt_for_password_only(const char *ssid, char *password, size_t password_size)
{
    int password_len = input_test_prompt_line("Nova senha: ", password, password_size, true);
    if (password_len < 0) {
        ESP_LOGE(TAG, "Failed to read password");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Updated password received for SSID '%s' (%s)", ssid, password_len > 0 ? "WPA/WPA2" : "open");
    return ESP_OK;
}

static int wifi_prompt_saved_credentials_action(const char *ssid)
{
    char command[16];

    printf("\nCredenciais salvas encontradas para '%s'\n", ssid);
    printf("1. Tentar com as credenciais salvas\n");
    printf("2. Selecionar outra rede/senha\n");
    printf("0. Voltar\n");

    while (true) {
        int command_len = input_test_prompt_line("Opcao: ", command, sizeof(command), false);
        if (command_len <= 0) {
            continue;
        }

        if (strcmp(command, "1") == 0) {
            return 1;
        }

        if (strcmp(command, "2") == 0) {
            return 2;
        }

        if (strcmp(command, "0") == 0) {
            return 0;
        }

        ESP_LOGW(TAG, "Unknown option '%s'", command);
    }
}

static int wifi_prompt_failed_connection_action(const char *ssid)
{
    char command[16];

    printf("\nFalha ao conectar em '%s'\n", ssid);
    printf("1. Tentar novamente\n");
    printf("2. Digitar outra senha\n");
    printf("3. Selecionar outra rede\n");
    printf("0. Voltar ao menu\n");

    while (true) {
        int command_len = input_test_prompt_line("Opcao: ", command, sizeof(command), false);
        if (command_len <= 0) {
            continue;
        }

        if (strcmp(command, "1") == 0) {
            return 1;
        }

        if (strcmp(command, "2") == 0) {
            return 2;
        }

        if (strcmp(command, "3") == 0) {
            return 3;
        }

        if (strcmp(command, "0") == 0) {
            return 0;
        }

        ESP_LOGW(TAG, "Unknown option '%s'", command);
    }
}

esp_err_t wifi_connect_from_console(void)
{
    char ssid[33];
    char password[65];
    bool has_saved_credentials = false;

    esp_err_t ret = input_test_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Console initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = wifi_stack_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi stack initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));

    ret = wifi_load_credentials_from_nvs(ssid, sizeof(ssid), password, sizeof(password));
    if (ret == ESP_OK) {
        has_saved_credentials = true;
        ESP_LOGI(TAG, "Loaded Wi-Fi credentials from protected NVS storage");
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Unable to read Wi-Fi credentials from NVS: %s", esp_err_to_name(ret));
        wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
    }

    if (has_saved_credentials) {
        int action = wifi_prompt_saved_credentials_action(ssid);
        if (action == 0) {
            wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
            return ESP_OK;
        }

        if (action == 2) {
            wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
            has_saved_credentials = false;
        }
    }

    if (!has_saved_credentials) {
        ret = wifi_prompt_for_credentials(ssid, sizeof(ssid), password, sizeof(password));
        if (ret != ESP_OK) {
            wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
            return ret;
        }
    }

    while (true) {
        ret = wifi_connect_with_credentials(ssid,
                                            password,
                                            pdMS_TO_TICKS(CONFIG_AUTOBOOT_WIFI_MANUAL_CONNECT_TIMEOUT_MS),
                                            false);
        if (ret == ESP_OK) {
            ret = wifi_save_credentials(ssid, password);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to store Wi-Fi credentials in NVS: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Wi-Fi credentials saved to protected NVS storage");
            }

            wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
            return ESP_OK;
        }

        int action = wifi_prompt_failed_connection_action(ssid);
        if (action == 0) {
            wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
            return ESP_FAIL;
        }

        if (action == 1) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        if (action == 2) {
            ret = wifi_prompt_for_password_only(ssid, password, sizeof(password));
            if (ret != ESP_OK) {
                wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
                return ret;
            }
            continue;
        }

        ret = wifi_prompt_for_credentials(ssid, sizeof(ssid), password, sizeof(password));
        if (ret != ESP_OK) {
            wifi_clear_credentials(ssid, sizeof(ssid), password, sizeof(password));
            return ret;
        }
    }
}

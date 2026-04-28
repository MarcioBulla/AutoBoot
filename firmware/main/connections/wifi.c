#include "connections/wifi.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "input_system/input_test.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_NAMESPACE     "wifi_cfg"
#define WIFI_KEY_SSID      "ssid_enc"
#define WIFI_KEY_PASSWORD  "pwd_enc"
#define WIFI_LEGACY_KEY_SSID     "ssid"
#define WIFI_LEGACY_KEY_PASSWORD "password"
#define WIFI_CRED_IV_LEN         8
#define WIFI_CRED_TAG_LEN        8
#define WIFI_CRED_MAX_TEXT_LEN   64
#define WIFI_SCAN_MAX_APS        32
#define WIFI_SCAN_PAGE_SIZE      5

static const uint8_t WIFI_CRED_SECRET[] = "AutoBoot:wifi-creds:v1";

typedef struct {
    uint8_t version;
    uint8_t plaintext_len;
    uint8_t iv[WIFI_CRED_IV_LEN];
    uint8_t ciphertext[WIFI_CRED_MAX_TEXT_LEN];
    uint8_t tag[WIFI_CRED_TAG_LEN];
} wifi_credential_blob_t;

static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_stack_ready = false;
static TaskHandle_t s_wifi_console_task_handle = NULL;
static wifi_err_reason_t s_last_disconnect_reason = WIFI_REASON_UNSPECIFIED;

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

static uint32_t wifi_fnv1a32(const uint8_t *data, size_t len, uint32_t seed)
{
    uint32_t hash = 2166136261u ^ seed;

    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }

    return hash;
}

static esp_err_t wifi_derive_device_key(uint32_t key[4])
{
    uint8_t mac[6];
    uint8_t material[sizeof(WIFI_CRED_SECRET) - 1 + sizeof(mac)];

    esp_err_t ret = esp_efuse_mac_get_default(mac);
    if (ret != ESP_OK) {
        return ret;
    }

    memcpy(material, WIFI_CRED_SECRET, sizeof(WIFI_CRED_SECRET) - 1);
    memcpy(material + sizeof(WIFI_CRED_SECRET) - 1, mac, sizeof(mac));

    key[0] = wifi_fnv1a32(material, sizeof(material), 0x13579bdfu);
    key[1] = wifi_fnv1a32(material, sizeof(material), 0x2468ace0u);
    key[2] = wifi_fnv1a32(material, sizeof(material), 0xdeadbeefu);
    key[3] = wifi_fnv1a32(material, sizeof(material), 0x0badc0deu);

    memset(material, 0, sizeof(material));
    memset(mac, 0, sizeof(mac));
    return ESP_OK;
}

static void wifi_xtea_encipher(uint32_t block[2], const uint32_t key[4])
{
    uint32_t v0 = block[0];
    uint32_t v1 = block[1];
    uint32_t sum = 0;
    const uint32_t delta = 0x9E3779B9u;

    for (int round = 0; round < 32; ++round) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }

    block[0] = v0;
    block[1] = v1;
}

static void wifi_fill_keystream_block(const uint32_t key[4],
                                      const uint8_t iv[WIFI_CRED_IV_LEN],
                                      uint32_t counter,
                                      uint8_t stream[8])
{
    uint32_t block[2];

    memcpy(&block[0], iv, sizeof(uint32_t));
    memcpy(&block[1], iv + sizeof(uint32_t), sizeof(uint32_t));
    block[0] ^= counter;
    block[1] ^= (counter * 0x9E3779B9u);
    wifi_xtea_encipher(block, key);
    memcpy(stream, block, sizeof(block));
    memset(block, 0, sizeof(block));
}

static void wifi_xor_crypt(const uint32_t key[4],
                           const uint8_t iv[WIFI_CRED_IV_LEN],
                           const uint8_t *input,
                           uint8_t *output,
                           size_t len)
{
    uint32_t counter = 0;
    size_t offset = 0;

    while (offset < len) {
        uint8_t stream[8];
        size_t chunk_len;

        wifi_fill_keystream_block(key, iv, counter++, stream);
        chunk_len = len - offset;
        if (chunk_len > sizeof(stream)) {
            chunk_len = sizeof(stream);
        }

        for (size_t i = 0; i < chunk_len; ++i) {
            output[offset + i] = input[offset + i] ^ stream[i];
        }

        memset(stream, 0, sizeof(stream));
        offset += chunk_len;
    }
}

static void wifi_compute_blob_tag(const uint32_t key[4],
                                  const wifi_credential_blob_t *blob,
                                  uint8_t tag[WIFI_CRED_TAG_LEN])
{
    uint8_t material[sizeof(uint32_t) * 4 + 2 + WIFI_CRED_IV_LEN + WIFI_CRED_MAX_TEXT_LEN];
    uint32_t digest_a;
    uint32_t digest_b;
    size_t material_len = 0;

    memcpy(material + material_len, key, sizeof(uint32_t) * 4);
    material_len += sizeof(uint32_t) * 4;
    material[material_len++] = blob->version;
    material[material_len++] = blob->plaintext_len;
    memcpy(material + material_len, blob->iv, WIFI_CRED_IV_LEN);
    material_len += WIFI_CRED_IV_LEN;
    memcpy(material + material_len, blob->ciphertext, WIFI_CRED_MAX_TEXT_LEN);
    material_len += WIFI_CRED_MAX_TEXT_LEN;

    digest_a = wifi_fnv1a32(material, material_len, 0xa5a5a5a5u);
    digest_b = wifi_fnv1a32(material, material_len, 0x5a5a5a5au);
    memcpy(tag, &digest_a, sizeof(digest_a));
    memcpy(tag + sizeof(digest_a), &digest_b, sizeof(digest_b));

    memset(material, 0, sizeof(material));
    digest_a = 0;
    digest_b = 0;
}

static bool wifi_tags_match(const uint8_t *lhs, const uint8_t *rhs, size_t len)
{
    uint8_t diff = 0;

    for (size_t i = 0; i < len; ++i) {
        diff |= lhs[i] ^ rhs[i];
    }

    return diff == 0;
}

static esp_err_t wifi_encrypt_text(const char *text, wifi_credential_blob_t *blob)
{
    uint32_t key[4];
    size_t text_len;
    esp_err_t ret;

    if (text == NULL || blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    text_len = strlen(text);
    if (text_len > WIFI_CRED_MAX_TEXT_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = wifi_derive_device_key(key);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(blob, 0, sizeof(*blob));
    blob->version = 1;
    blob->plaintext_len = (uint8_t)text_len;
    esp_fill_random(blob->iv, sizeof(blob->iv));
    wifi_xor_crypt(key, blob->iv, (const uint8_t *)text, blob->ciphertext, text_len);
    wifi_compute_blob_tag(key, blob, blob->tag);

    memset(key, 0, sizeof(key));
    return ESP_OK;
}

static esp_err_t wifi_decrypt_text(const wifi_credential_blob_t *blob, char *buffer, size_t buffer_size)
{
    uint32_t key[4];
    uint8_t expected_tag[WIFI_CRED_TAG_LEN];
    esp_err_t ret;

    if (blob == NULL || buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (blob->version != 1 || blob->plaintext_len >= buffer_size || blob->plaintext_len > WIFI_CRED_MAX_TEXT_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = wifi_derive_device_key(key);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_compute_blob_tag(key, blob, expected_tag);
    if (!wifi_tags_match(blob->tag, expected_tag, sizeof(expected_tag))) {
        memset(key, 0, sizeof(key));
        memset(expected_tag, 0, sizeof(expected_tag));
        return ESP_ERR_INVALID_CRC;
    }

    wifi_xor_crypt(key, blob->iv, blob->ciphertext, (uint8_t *)buffer, blob->plaintext_len);
    buffer[blob->plaintext_len] = '\0';

    memset(key, 0, sizeof(key));
    memset(expected_tag, 0, sizeof(expected_tag));
    return ESP_OK;
}

static void wifi_clear_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    memset(ssid, 0, ssid_size);
    memset(password, 0, password_size);
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        s_last_disconnect_reason = disconnected->reason;

        ESP_LOGW(TAG, "Disconnected from AP, reason=%d", disconnected->reason);
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_last_disconnect_reason = WIFI_REASON_UNSPECIFIED;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_stack_init(void)
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

static esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    wifi_credential_blob_t ssid_blob;
    wifi_credential_blob_t password_blob;
    nvs_handle_t handle;
    esp_err_t ret = wifi_encrypt_text(ssid, &ssid_blob);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = wifi_encrypt_text(password, &password_blob);
    if (ret != ESP_OK) {
        memset(&ssid_blob, 0, sizeof(ssid_blob));
        return ret;
    }

    ret = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        memset(&ssid_blob, 0, sizeof(ssid_blob));
        memset(&password_blob, 0, sizeof(password_blob));
        return ret;
    }

    ret = nvs_set_blob(handle, WIFI_KEY_SSID, &ssid_blob, sizeof(ssid_blob));
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, WIFI_KEY_PASSWORD, &password_blob, sizeof(password_blob));
    }

    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, WIFI_LEGACY_KEY_SSID);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }

    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, WIFI_LEGACY_KEY_PASSWORD);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    memset(&ssid_blob, 0, sizeof(ssid_blob));
    memset(&password_blob, 0, sizeof(password_blob));
    return ret;
}

static esp_err_t wifi_erase_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_key(handle, WIFI_KEY_SSID);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }

    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, WIFI_KEY_PASSWORD);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }

    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, WIFI_LEGACY_KEY_SSID);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }

    if (ret == ESP_OK) {
        ret = nvs_erase_key(handle, WIFI_LEGACY_KEY_PASSWORD);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }

    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
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

    ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, records);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_sort_ap_records(records, ap_count);
    *record_count = wifi_compact_unique_ssids(records, ap_count);
    return ESP_OK;
}

static esp_err_t wifi_load_legacy_credentials(nvs_handle_t handle,
                                              char *ssid,
                                              size_t ssid_size,
                                              char *password,
                                              size_t password_size)
{
    size_t ssid_len = ssid_size;
    esp_err_t ret = nvs_get_str(handle, WIFI_LEGACY_KEY_SSID, ssid, &ssid_len);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t password_len = password_size;
    return nvs_get_str(handle, WIFI_LEGACY_KEY_PASSWORD, password, &password_len);
}

static esp_err_t wifi_load_credentials_from_nvs(char *ssid,
                                                size_t ssid_size,
                                                char *password,
                                                size_t password_size)
{
    wifi_credential_blob_t ssid_blob;
    wifi_credential_blob_t password_blob;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t blob_size = sizeof(ssid_blob);
    ret = nvs_get_blob(handle, WIFI_KEY_SSID, &ssid_blob, &blob_size);
    if (ret == ESP_OK) {
        blob_size = sizeof(password_blob);
        ret = nvs_get_blob(handle, WIFI_KEY_PASSWORD, &password_blob, &blob_size);
    }

    if (ret == ESP_OK) {
        ret = wifi_decrypt_text(&ssid_blob, ssid, ssid_size);
        if (ret == ESP_OK) {
            ret = wifi_decrypt_text(&password_blob, password, password_size);
        }
    } else if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = wifi_load_legacy_credentials(handle, ssid, ssid_size, password, password_size);
    }

    nvs_close(handle);
    memset(&ssid_blob, 0, sizeof(ssid_blob));
    memset(&password_blob, 0, sizeof(password_blob));
    return ret;
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
    size_t start = page_index * WIFI_SCAN_PAGE_SIZE;
    size_t end = start + WIFI_SCAN_PAGE_SIZE;

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
    printf("n = proximas 5 | p = 5 anteriores | r = escanear de novo | m = SSID manual\n");
}

static esp_err_t wifi_select_scanned_ssid(char *ssid, size_t ssid_size)
{
    wifi_ap_record_t records[WIFI_SCAN_MAX_APS];
    char command[16];
    size_t record_count = 0;
    size_t page_index = 0;
    esp_err_t ret;

    while (true) {
        ret = wifi_scan_access_points(records, WIFI_SCAN_MAX_APS, &record_count);
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
            size_t start = page_index * WIFI_SCAN_PAGE_SIZE;
            size_t end = start + WIFI_SCAN_PAGE_SIZE;

            if (end > record_count) {
                end = record_count;
            }

            wifi_print_scan_page(records, record_count, page_index);

            int command_len = input_test_prompt_line("Escolha [1-5/n/p/r/m]: ",
                                                     command,
                                                     sizeof(command),
                                                     false);
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

static esp_err_t wifi_connect_with_credentials(const char *ssid, const char *password)
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

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected successfully");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi connection timed out or failed (reason=%d)", s_last_disconnect_reason);
    return ESP_FAIL;
}

static void wifi_print_status(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    if (ret == ESP_OK) {
        printf("Wi-Fi conectado em '%s' | RSSI %d\n", (const char *)ap_info.ssid, ap_info.rssi);
        return;
    }

    printf("Wi-Fi desconectado\n");
}

static void wifi_console_task(void *arg)
{
    char trigger[8];
    char command[16];

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

        while (true) {
            printf("\nMenu Wi-Fi\n");
            printf("1. Conectar/configurar Wi-Fi\n");
            printf("2. Limpar credenciais salvas\n");
            printf("3. Mostrar status\n");
            printf("0. Voltar\n");

            int command_len = input_test_prompt_line("Opcao: ", command, sizeof(command), false);
            if (command_len <= 0) {
                ESP_LOGW(TAG, "Invalid menu option");
                continue;
            }

            if (strcmp(command, "1") == 0) {
                esp_err_t ret = wifi_connect_from_console();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Wi-Fi connection flow failed: %s", esp_err_to_name(ret));
                }
                continue;
            }

            if (strcmp(command, "2") == 0) {
                esp_err_t ret = wifi_erase_credentials();
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
                break;
            }

            ESP_LOGW(TAG, "Unknown menu option '%s'", command);
        }
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
        ret = wifi_connect_with_credentials(ssid, password);
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

esp_err_t wifi_start_console_task(void)
{
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

    if (s_wifi_console_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t task_ok = xTaskCreate(wifi_console_task,
                                     "wifi_console",
                                     6144,
                                     NULL,
                                     5,
                                     &s_wifi_console_task_handle);
    if (task_ok != pdPASS) {
        s_wifi_console_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

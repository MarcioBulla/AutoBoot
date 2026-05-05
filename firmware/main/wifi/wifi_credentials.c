#include "wifi/wifi.h"
#include "wifi/wifi_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

#define WIFI_NAMESPACE           "wifi_cfg"
#define WIFI_KEY_SSID            "ssid_enc"
#define WIFI_KEY_PASSWORD        "pwd_enc"
#define WIFI_LEGACY_KEY_SSID     "ssid"
#define WIFI_LEGACY_KEY_PASSWORD "password"
#define WIFI_CRED_IV_LEN         8
#define WIFI_CRED_TAG_LEN        8
#define WIFI_CRED_MAX_TEXT_LEN   64

static const uint8_t WIFI_CRED_SECRET[] = "AutoBoot:wifi-creds:v1";

typedef struct {
    uint8_t version;
    uint8_t plaintext_len;
    uint8_t iv[WIFI_CRED_IV_LEN];
    uint8_t ciphertext[WIFI_CRED_MAX_TEXT_LEN];
    uint8_t tag[WIFI_CRED_TAG_LEN];
} wifi_credential_blob_t;

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

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
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

esp_err_t wifi_erase_saved_credentials(void)
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

esp_err_t wifi_load_credentials_from_nvs(char *ssid,
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

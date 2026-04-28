#include "input_system/storage_text.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define STORAGE_TEXT_IO_BUFFER_SIZE (STORAGE_TEXT_MAX_LEN + 1)

static esp_err_t validate_text_input(const char *path, const char *text)
{
    if (path == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strnlen(text, STORAGE_TEXT_MAX_LEN + 1) > STORAGE_TEXT_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t storage_text_write(const char *path, const char *text)
{
    esp_err_t ret = validate_text_input(path, text);
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return ESP_FAIL;
    }

    size_t text_len = strlen(text);
    size_t written = fwrite(text, 1, text_len, file);

    if (fclose(file) != 0) {
        return ESP_FAIL;
    }

    if (written != text_len) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t storage_text_read(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (buffer_size < STORAGE_TEXT_IO_BUFFER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    size_t read_len = fread(buffer, 1, STORAGE_TEXT_MAX_LEN, file);
    if (ferror(file)) {
        fclose(file);
        return ESP_FAIL;
    }

    int next_char = fgetc(file);
    if (fclose(file) != 0) {
        return ESP_FAIL;
    }

    if (next_char != EOF) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[read_len] = '\0';
    return ESP_OK;
}

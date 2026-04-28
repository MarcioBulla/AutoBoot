#ifndef INPUT_SYSTEM_STORAGE_TEXT_H
#define INPUT_SYSTEM_STORAGE_TEXT_H

#include <stddef.h>

#include "esp_err.h"

#define STORAGE_TEXT_MAX_LEN 64

esp_err_t storage_text_write(const char *path, const char *text);
esp_err_t storage_text_read(const char *path, char *buffer, size_t buffer_size);

#endif

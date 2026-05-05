#ifndef WIFI_WIFI_H
#define WIFI_WIFI_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t wifi_init(void);
esp_err_t wifi_connect_from_console(void);
esp_err_t wifi_erase_saved_credentials(void);
void wifi_print_status(void);
esp_err_t wifi_start_boot_auto_connect(void);
void wifi_cancel_boot_auto_connect(void);
esp_err_t wifi_wait_boot_auto_connect_stopped(uint32_t timeout_ms);

#endif

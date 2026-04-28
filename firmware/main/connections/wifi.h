#ifndef CONNECTIONS_WIFI_H
#define CONNECTIONS_WIFI_H

#include "esp_err.h"

esp_err_t wifi_connect_from_console(void);
esp_err_t wifi_start_console_task(void);

#endif

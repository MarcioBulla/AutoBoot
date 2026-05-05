#ifndef WIFI_INTERNAL_H
#define WIFI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

void wifi_clear_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size);
esp_err_t wifi_stack_init(void);
void wifi_status_led_set(bool enabled);
void wifi_set_disconnect_expected(bool expected);
esp_err_t wifi_connect_with_credentials(const char *ssid,
                                        const char *password,
                                        TickType_t timeout_ticks,
                                        bool cancel_on_console_request);
bool wifi_boot_connect_should_cancel(void);

esp_err_t wifi_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_load_credentials_from_nvs(char *ssid,
                                         size_t ssid_size,
                                         char *password,
                                         size_t password_size);

#endif

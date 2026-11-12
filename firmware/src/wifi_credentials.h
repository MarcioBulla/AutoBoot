#ifndef AUTOBOOT_WIFI_CREDENTIALS_H_
#define AUTOBOOT_WIFI_CREDENTIALS_H_

#include <stdbool.h>
#include <stddef.h>

int wifi_credentials_init(void);
bool wifi_credentials_available(void);
const char *wifi_credentials_ssid(size_t *length);
const char *wifi_credentials_password(size_t *length);
int wifi_credentials_set(const char *ssid, size_t ssid_length,
			 const char *password, size_t password_length);

#endif /* AUTOBOOT_WIFI_CREDENTIALS_H_ */

#include <errno.h>
#include <string.h>

#include <zephyr/settings/settings.h>

#include "wifi_credentials.h"

#define WIFI_SSID_MAX_LENGTH 32
#define WIFI_PASSWORD_MAX_LENGTH 63

static char stored_ssid[WIFI_SSID_MAX_LENGTH + 1];
static char stored_password[WIFI_PASSWORD_MAX_LENGTH + 1];

static int wifi_settings_set(const char *name, size_t len,
			     settings_read_cb read_cb, void *cb_arg)
{
	char *destination;
	size_t destination_size;

	if (strcmp(name, "ssid") == 0) {
		destination = stored_ssid;
		destination_size = sizeof(stored_ssid);
	} else if (strcmp(name, "password") == 0) {
		destination = stored_password;
		destination_size = sizeof(stored_password);
	} else {
		return -ENOENT;
	}
	if (len == 0 || len >= destination_size || read_cb(cb_arg, destination, len) < 0) {
		return -EINVAL;
	}
	destination[len] = '\0';
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(autoboot_wifi, "autoboot_wifi", NULL,
			       wifi_settings_set, NULL, NULL);

int wifi_credentials_init(void)
{
	int err = settings_subsys_init();

	if (err != 0) {
		return err;
	}
	return settings_load_subtree("autoboot_wifi");
}

bool wifi_credentials_available(void)
{
	return stored_ssid[0] != '\0' && stored_password[0] != '\0';
}

const char *wifi_credentials_ssid(size_t *length)
{
	*length = strlen(stored_ssid);
	return stored_ssid;
}

const char *wifi_credentials_password(size_t *length)
{
	*length = strlen(stored_password);
	return stored_password;
}

int wifi_credentials_set(const char *ssid, size_t ssid_length,
			 const char *password, size_t password_length)
{
	char verify_ssid[WIFI_SSID_MAX_LENGTH];
	char verify_password[WIFI_PASSWORD_MAX_LENGTH];
	int err;

	if (ssid_length == 0 || ssid_length > WIFI_SSID_MAX_LENGTH ||
	    password_length < 8 || password_length > WIFI_PASSWORD_MAX_LENGTH) {
		return -EINVAL;
	}
	err = settings_save_one("autoboot_wifi/ssid", ssid, ssid_length);
	if (err != 0) {
		return err;
	}
	err = settings_save_one("autoboot_wifi/password", password, password_length);
	if (err != 0) {
		return err;
	}
	if (settings_load_one("autoboot_wifi/ssid", verify_ssid,
			      sizeof(verify_ssid)) != (ssize_t)ssid_length ||
	    memcmp(verify_ssid, ssid, ssid_length) != 0 ||
	    settings_load_one("autoboot_wifi/password", verify_password,
			      sizeof(verify_password)) != (ssize_t)password_length ||
	    memcmp(verify_password, password, password_length) != 0) {
		return -EIO;
	}

	memcpy(stored_ssid, ssid, ssid_length);
	stored_ssid[ssid_length] = '\0';
	memcpy(stored_password, password, password_length);
	stored_password[password_length] = '\0';
	return 0;
}

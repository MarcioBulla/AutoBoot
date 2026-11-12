#ifndef AUTOBOOT_MQTT_CLIENT_H_
#define AUTOBOOT_MQTT_CLIENT_H_

#include <stdbool.h>

int autoboot_mqtt_init(void);
void autoboot_mqtt_poll(bool wifi_connected);
int autoboot_mqtt_configure(const char *host, uint16_t port);

#endif

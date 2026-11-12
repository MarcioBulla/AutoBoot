#ifndef AUTOBOOT_CERTIFICATE_H_
#define AUTOBOOT_CERTIFICATE_H_

#include <stddef.h>
#include <zephyr/net/tls_credentials.h>

int certificate_init(void);
const char *certificate_csr(size_t *length);
int certificate_install_client(const char *pem, size_t length);
int certificate_install_ca(const char *pem, size_t length);
int certificate_register_mqtt_tls_credentials(sec_tag_t tag);

#endif /* AUTOBOOT_CERTIFICATE_H_ */

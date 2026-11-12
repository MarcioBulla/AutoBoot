#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/psa/key_ids.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/tls_credentials.h>

#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>

#include "certificate.h"

LOG_MODULE_REGISTER(certificate, LOG_LEVEL_INF);

#define CERTIFICATE_CSR_PEM_MAX 1024
#define CERTIFICATE_DEVICE_KEY_ID ZEPHYR_PSA_APPLICATION_KEY_ID_RANGE_BEGIN
#define CERTIFICATE_TLS_PEM_MAX 4096

static char csr_pem[CERTIFICATE_CSR_PEM_MAX];
static char mqtt_client_pem[CERTIFICATE_TLS_PEM_MAX + 1];
static char mqtt_ca_pem[CERTIFICATE_TLS_PEM_MAX + 1];

static int certificate_settings_set(const char *name, size_t len,
				    settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "csr") != 0 && strcmp(name, "client") != 0 &&
	    strcmp(name, "ca") != 0) {
		return -ENOENT;
	}
	if (len == 0) {
		return -EFBIG;
	}
	/* Client and CA values are deliberately not kept in RAM after boot. */
	if (strcmp(name, "csr") == 0) {
		if (len >= sizeof(csr_pem) || read_cb(cb_arg, csr_pem, len) < 0) {
			return -EIO;
		}
		csr_pem[len] = '\0';
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(autoboot_cert, "autoboot_cert", NULL,
			       certificate_settings_set, NULL, NULL);

static int ensure_device_key(void)
{
	psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t generated_key_id;
	psa_status_t status;

	status = psa_get_key_attributes(CERTIFICATE_DEVICE_KEY_ID, &attributes);
	if (status == PSA_SUCCESS) {
		psa_reset_key_attributes(&attributes);
		return 0;
	}
	/* PSA Crypto reports an unknown persistent key as INVALID_HANDLE on this
	 * implementation; some PSA ITS implementations return DOES_NOT_EXIST.
	 */
	if (status != PSA_ERROR_DOES_NOT_EXIST && status != PSA_ERROR_INVALID_HANDLE) {
		LOG_ERR("Device key lookup failed (%d)", status);
		return -EIO;
	}

	psa_set_key_id(&attributes, CERTIFICATE_DEVICE_KEY_ID);
	psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_PERSISTENT);
	psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attributes, 256);
	psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
	psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	status = psa_generate_key(&attributes, &generated_key_id);
	psa_reset_key_attributes(&attributes);
	if (status != PSA_SUCCESS) {
		LOG_ERR("Device key generation failed (%d)", status);
		return -EIO;
	}

	LOG_INF("Generated persistent device private key");
	return 0;
}

static int generate_csr(void)
{
	mbedtls_x509write_csr request;
	mbedtls_pk_context key;
	int err;

	mbedtls_x509write_csr_init(&request);
	mbedtls_pk_init(&key);
	mbedtls_x509write_csr_set_md_alg(&request, MBEDTLS_MD_SHA256);
	err = mbedtls_x509write_csr_set_subject_name(&request, "CN=AutoBoot");
	if (err == 0) {
		err = mbedtls_pk_wrap_psa(&key, CERTIFICATE_DEVICE_KEY_ID);
	}
	if (err == 0) {
		mbedtls_x509write_csr_set_key(&request, &key);
		err = mbedtls_x509write_csr_pem(&request, (unsigned char *)csr_pem,
					       sizeof(csr_pem));
	}

	mbedtls_pk_free(&key);
	mbedtls_x509write_csr_free(&request);
	return err;
}

int certificate_init(void)
{
	psa_status_t status;
	int err;

	err = settings_subsys_init();
	if (err != 0) {
		LOG_ERR("Settings initialization failed (%d)", err);
		return err;
	}
	err = settings_load_subtree("autoboot_cert");
	if (err != 0) {
		LOG_ERR("Certificate settings load failed (%d)", err);
		return err;
	}

	status = psa_crypto_init();
	if (status != PSA_SUCCESS) {
		LOG_ERR("PSA crypto initialization failed (%d)", status);
		return -EIO;
	}
	err = ensure_device_key();
	if (err != 0) {
		return err;
	}
	if (csr_pem[0] != '\0') {
		return 0;
	}

	err = generate_csr();
	if (err != 0) {
		LOG_ERR("CSR generation failed (%d)", err);
		return err;
	}
	err = settings_save_one("autoboot_cert/csr", csr_pem, strlen(csr_pem));
	if (err == 0) {
		LOG_INF("Generated persistent device CSR");
	} else {
		LOG_ERR("CSR storage failed (%d)", err);
	}

	return err;
}

const char *certificate_csr(size_t *length)
{
	if (length != NULL) {
		*length = strlen(csr_pem);
	}
	return csr_pem;
}

static int certificate_validate(const char *pem, size_t length, bool require_device_key)
{
	mbedtls_x509_crt certificate;
	mbedtls_pk_context device_key;
	int err;

	if (length == 0 || pem == NULL || memchr(pem, '\0', length) != NULL) {
		return -EINVAL;
	}

	mbedtls_x509_crt_init(&certificate);
	mbedtls_pk_init(&device_key);
	err = mbedtls_x509_crt_parse(&certificate, (const unsigned char *)pem,
				    length + 1U);
	if (err != 0) {
		LOG_ERR("X.509 PEM parse failed: -0x%x", (unsigned int)-err);
	}
	if (err == 0 && require_device_key) {
		err = mbedtls_pk_wrap_psa(&device_key, CERTIFICATE_DEVICE_KEY_ID);
		if (err != 0) {
			LOG_ERR("Could not access the PSA device key: -0x%x", (unsigned int)-err);
		}
	}
	if (err == 0 && require_device_key) {
		err = mbedtls_pk_check_pair(&certificate.pk, &device_key);
		if (err != 0) {
			LOG_ERR("Client certificate public key differs from device key: -0x%x",
				(unsigned int)-err);
		}
	}
	mbedtls_pk_free(&device_key);
	mbedtls_x509_crt_free(&certificate);
	return err == 0 ? 0 : -EINVAL;
}

static int certificate_install(const char *name, const char *pem, size_t length,
			       bool require_device_key)
{
	int err = certificate_validate(pem, length, require_device_key);

	if (err != 0) {
		LOG_ERR("Rejected invalid %s certificate", name);
		return err;
	}
	/* Validation completes before NVS is touched: a valid prior value survives. */
	err = settings_save_one(name, pem, length);
	if (err != 0) {
		LOG_ERR("Unable to store %s certificate (%d)", name, err);
	}
	return err;
}

int certificate_install_client(const char *pem, size_t length)
{
	return certificate_install("autoboot_cert/client", pem, length, true);
}

int certificate_install_ca(const char *pem, size_t length)
{
	return certificate_install("autoboot_cert/ca", pem, length, false);
}

static int certificate_load_tls_pem(const char *name, char *buffer, size_t capacity,
				    size_t *length)
{
	ssize_t count = settings_load_one(name, buffer, capacity - 1U);

	if (count <= 0) {
		return count == 0 ? -ENOENT : (int)count;
	}
	if ((size_t)count >= capacity) {
		return -EFBIG;
	}
	buffer[count] = '\0';
	*length = (size_t)count + 1U;
	return 0;
}

int certificate_register_mqtt_tls_credentials(sec_tag_t tag)
{
	static bool registered;
	/* The volatile TLS credential backend retains this address rather than
	 * copying the key id. Keep it valid for the entire socket lifetime. */
	static const psa_key_id_t key_id = CERTIFICATE_DEVICE_KEY_ID;
	size_t client_len;
	size_t ca_len;
	int err;

	if (registered) {
		return 0;
	}
	err = certificate_load_tls_pem("autoboot_cert/client", mqtt_client_pem,
				       sizeof(mqtt_client_pem), &client_len);
	if (err != 0) {
		LOG_ERR("Client certificate is not provisioned (%d)", err);
		return err;
	}
	err = certificate_load_tls_pem("autoboot_cert/ca", mqtt_ca_pem,
				       sizeof(mqtt_ca_pem), &ca_len);
	if (err != 0) {
		LOG_ERR("MQTT server CA is not provisioned (%d)", err);
		return err;
	}

	err = tls_credential_add(tag, TLS_CREDENTIAL_CA_CERTIFICATE,
				 mqtt_ca_pem, ca_len);
	if (err != 0 && err != -EEXIST) {
		return err;
	}
	err = tls_credential_add(tag, TLS_CREDENTIAL_PUBLIC_CERTIFICATE,
				 mqtt_client_pem, client_len);
	if (err != 0 && err != -EEXIST) {
		return err;
	}
	err = tls_credential_add(tag, TLS_CREDENTIAL_PRIVATE_KEY_PSA,
				 &key_id, sizeof(key_id));
	if (err != 0 && err != -EEXIST) {
		return err;
	}
	registered = true;
	return 0;
}

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "certificate.h"
#include "mqtt_client.h"
#include "usb_maintenance.h"
#include "wifi_credentials.h"

LOG_MODULE_REGISTER(usb_maintenance, LOG_LEVEL_INF);

#define USB_LINE_MAX 128
#define USB_CERT_PEM_MAX 4096

static const struct device *const usb_uart = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
/* One reusable buffer: certificates are validated before replacing flash storage. */
static char transfer_buffer[USB_CERT_PEM_MAX + 1];
static struct k_sem tx_queued;
static struct k_mutex tx_lock;
static const uint8_t *tx_data;
static size_t tx_length;
static size_t tx_offset;
RING_BUF_DECLARE(usb_rx, 512);
static struct k_spinlock usb_io_lock;
static bool maintenance_initialized;
static char input_line[USB_LINE_MAX];
static size_t input_length;

static void usb_tx_callback(const struct device *device, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(device);
	k_spinlock_key_t key = k_spin_lock(&usb_io_lock);
	if (uart_irq_rx_ready(device)) {
		uint8_t bytes[64];
		int count;
		while (ring_buf_space_get(&usb_rx) != 0U) {
			count = uart_fifo_read(device, bytes,
				MIN(sizeof(bytes), ring_buf_space_get(&usb_rx)));
			if (count <= 0) {
				break;
			}
			(void)ring_buf_put(&usb_rx, bytes, count);
		}
		if (ring_buf_space_get(&usb_rx) == 0U) {
			uart_irq_rx_disable(device);
		}
	}
	if (!uart_irq_tx_ready(device) || tx_offset >= tx_length) {
		k_spin_unlock(&usb_io_lock, key);
		return;
	}

	int written = uart_fifo_fill(device, &tx_data[tx_offset],
				    tx_length - tx_offset);

	if (written <= 0) {
		k_spin_unlock(&usb_io_lock, key);
		return;
	}
	tx_offset += (size_t)written;
	if (tx_offset == tx_length) {
		uart_irq_tx_disable(device);
		k_sem_give(&tx_queued);
	}
	k_spin_unlock(&usb_io_lock, key);
}

static int usb_read_byte(uint8_t *byte)
{
	k_spinlock_key_t key = k_spin_lock(&usb_io_lock);
	uint32_t count = ring_buf_get(&usb_rx, byte, 1);
	k_spin_unlock(&usb_io_lock, key);
	if (count != 0U) {
		uart_irq_rx_enable(usb_uart);
	}
	return count == 1U ? 0 : -EAGAIN;
}

static void write_bytes(const char *buffer, size_t length)
{
	if (length == 0) {
		return;
	}

	/* uart_fifo_fill() runs in the CDC ACM work queue and schedules TX. */
	k_mutex_lock(&tx_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&usb_io_lock);
	tx_data = (const uint8_t *)buffer;
	tx_length = length;
	tx_offset = 0;
	k_sem_reset(&tx_queued);
	uart_irq_tx_enable(usb_uart);
	k_spin_unlock(&usb_io_lock, key);
	if (k_sem_take(&tx_queued, K_SECONDS(5)) != 0) {
		LOG_ERR("USB transmit queue timeout");
		uart_irq_tx_disable(usb_uart);
	}
	key = k_spin_lock(&usb_io_lock);
	uart_irq_tx_disable(usb_uart);
	tx_length = 0;
	tx_data = NULL;
	k_spin_unlock(&usb_io_lock, key);
	k_mutex_unlock(&tx_lock);
}

static void reply(const char *text)
{
	write_bytes(text, strlen(text));
}

static int read_byte(uint8_t *byte, int64_t deadline)
{
	int err;

	do {
		err = usb_read_byte(byte);
		if (err != 0) {
			if (k_uptime_get() >= deadline) {
				return -ETIMEDOUT;
			}
			k_msleep(5);
		}
	} while (err != 0);
	return 0;
}

static int read_payload(size_t length)
{
	uint8_t byte;
	int64_t deadline = k_uptime_get() + 10000;

	if (length == 0 || length > USB_CERT_PEM_MAX) {
		return -EFBIG;
	}
	for (size_t i = 0; i < length; ++i) {
		int err = read_byte(&byte, deadline);
		if (err != 0) {
			LOG_WRN("USB upload timed out after %u of %u bytes",
				(unsigned int)i, (unsigned int)length);
			return err;
		}
		transfer_buffer[i] = (char)byte;
	}
	transfer_buffer[length] = '\0';
	return 0;
}

static void show_csr(void)
{
	size_t length;
	const char *csr = certificate_csr(&length);
	char header[32];

	if (length == 0) {
		reply("ERR csr-unavailable\n");
		return;
	}

	snprintf(header, sizeof(header), "BEGIN CSR %u\n", (unsigned int)length);
	reply(header);
	write_bytes(csr, length);
	reply("END CSR\n");
}

static void handle_put(const char *kind, const char *size_string)
{
	char *end;
	unsigned long length = strtoul(size_string, &end, 10);
	int err;

	if (*size_string == '\0' || *end != '\0') {
		reply("ERR size\n");
		return;
	}
	if (length > USB_CERT_PEM_MAX) {
		reply("ERR too-large\n");
		return;
	}
	reply("READY\n");
	err = read_payload((size_t)length);
	if (err == 0 && strcmp(kind, "CLIENT") == 0) {
		err = certificate_install_client(transfer_buffer, (size_t)length);
	} else if (err == 0) {
		err = certificate_install_ca(transfer_buffer, (size_t)length);
	}
	reply(err == 0 ? "OK stored\n" : "ERR rejected\n");
}

static void handle_command(char *line)
{
	char *argument = strchr(line, ' ');

	if (argument != NULL) {
		*argument++ = '\0';
	}
	if (strcmp(line, "HELP") == 0) {
		reply("CSR | WIFI <ssid> <password> | MQTT <host> <port> | PUT CLIENT <bytes> | PUT CA <bytes>\n");
	} else if (strcmp(line, "CSR") == 0 && argument == NULL) {
		show_csr();
	} else if (strcmp(line, "PUT") == 0 && argument != NULL) {
		char *size = strchr(argument, ' ');

		if (size == NULL) {
			reply("ERR syntax\n");
			return;
		}
		*size++ = '\0';
		if (strcmp(argument, "CLIENT") == 0 || strcmp(argument, "CA") == 0) {
			handle_put(argument, size);
		} else {
			reply("ERR type\n");
		}
	} else if (strcmp(line, "WIFI") == 0 && argument != NULL) {
		char *password = strchr(argument, ' ');
		int err;

		if (password == NULL) {
			reply("ERR syntax\n");
			return;
		}
		*password++ = '\0';
		err = wifi_credentials_set(argument, strlen(argument), password,
					   strlen(password));
		reply(err == 0 ? "OK wifi-saved; reboot\n" : "ERR wifi\n");
	} else if (strcmp(line, "MQTT") == 0 && argument != NULL) {
		char *port_string = strchr(argument, ' ');
		char *end;
		unsigned long port;
		int err;

		if (port_string == NULL) {
			reply("ERR syntax\n");
			return;
		}
		*port_string++ = '\0';
		port = strtoul(port_string, &end, 10);
		if (*port_string == '\0' || *end != '\0' || port == 0U || port > UINT16_MAX) {
			reply("ERR port\n");
			return;
		}
		err = autoboot_mqtt_configure(argument, (uint16_t)port);
		reply(err == 0 ? "OK mqtt-saved\n" : "ERR mqtt\n");
	} else {
		reply("ERR command\n");
	}
}

void usb_maintenance_poll(void)
{
	uint8_t byte;

	if (!maintenance_initialized && !device_is_ready(usb_uart)) {
		LOG_ERR("USB CDC ACM device is unavailable");
		return;
	}
	if (!maintenance_initialized) {
		k_sem_init(&tx_queued, 0, 1);
		k_mutex_init(&tx_lock);
		uart_irq_callback_user_data_set(usb_uart, usb_tx_callback, NULL);
		uart_irq_rx_enable(usb_uart);
		maintenance_initialized = true;
		LOG_INF("USB maintenance ready");
	}
	while (usb_read_byte(&byte) == 0) {
		if (byte == '\r') {
			continue;
		}
		if (byte == '\n') {
			input_line[input_length] = '\0';
			handle_command(input_line);
			input_length = 0;
			continue;
		}
		if (input_length + 1U >= sizeof(input_line)) {
			input_length = 0;
			reply("ERR line-too-long\n");
			continue;
		}
		input_line[input_length++] = (char)byte;
	}
}

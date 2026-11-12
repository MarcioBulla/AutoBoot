#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>

#include "certificate.h"
#include "mqtt_client.h"
#include "usb_maintenance.h"
#include "wifi_credentials.h"

LOG_MODULE_REGISTER(autoboot, LOG_LEVEL_INF);

static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_NODELABEL(user_led), gpios);
static struct k_work_delayable led_blink_work;
static struct k_work_delayable wifi_connect_work;
static atomic_t wifi_connected;
static struct net_if *wifi_iface;
static struct wifi_connect_req_params wifi_connect_params;

static void set_status_led(bool enabled)
{
	/* gpio_pin_set_dt() honors GPIO_ACTIVE_HIGH and GPIO_ACTIVE_LOW from DTS. */
	(void)gpio_pin_set_dt(&status_led, enabled);
}

static void led_blink_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_get(&wifi_connected)) {
		set_status_led(true);
		return;
	}

	(void)gpio_pin_toggle_dt(&status_led);
	(void)k_work_reschedule(&led_blink_work, K_MSEC(125));
}

static void set_wifi_connection_state(bool connected)
{
	atomic_set(&wifi_connected, connected);

	if (connected) {
		(void)k_work_cancel_delayable(&led_blink_work);
		set_status_led(true);
	} else {
		(void)k_work_reschedule(&led_blink_work, K_NO_WAIT);
	}
}

static void wifi_event_handler(struct net_mgmt_event_callback *callback,
			       uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = callback->info;

	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		if (status != NULL && status->status == 0) {
			set_wifi_connection_state(true);
			LOG_INF("Wi-Fi connected");
		} else {
			set_wifi_connection_state(false);
			LOG_WRN("Wi-Fi attempt failed (%d); driver will retry",
				status != NULL ? status->status : -1);
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		set_wifi_connection_state(false);
		LOG_WRN("Wi-Fi disconnected");
		break;
	default:
		break;
	}
}

static void wifi_connect_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);
	LOG_INF("Connecting to configured Wi-Fi network");
	err = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &wifi_connect_params,
		       sizeof(wifi_connect_params));
	if (err != 0) {
		LOG_ERR("Unable to request Wi-Fi connection (%d)", err);
	}
}

int main(void)
{
	static struct net_mgmt_event_callback wifi_callback;
	size_t ssid_length;
	size_t password_length;
	int err;

	err = certificate_init();
	if (err != 0) {
		LOG_ERR("Certificate provisioning is unavailable (%d)", err);
	}
	err = wifi_credentials_init();
	if (err != 0) {
		LOG_ERR("Wi-Fi credential storage is unavailable (%d)", err);
	}
	err = autoboot_mqtt_init();
	if (err != 0) {
		LOG_ERR("MQTT initialization failed (%d)", err);
	}

	k_work_init_delayable(&led_blink_work, led_blink_work_handler);
	if (!gpio_is_ready_dt(&status_led) ||
	    gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_ERR("Status LED is unavailable");
	}

	wifi_iface = net_if_get_wifi_sta();
	if (wifi_iface == NULL) {
		LOG_ERR("Wi-Fi station interface is unavailable");
		while (true) {
			usb_maintenance_poll();
			autoboot_mqtt_poll(false);
			k_msleep(20);
		}
	}

	net_mgmt_init_event_callback(&wifi_callback, wifi_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_callback);
	if (!wifi_credentials_available()) {
		LOG_WRN("Wi-Fi is not configured; use the USB provisioning port");
		while (true) {
			usb_maintenance_poll();
			autoboot_mqtt_poll(false);
			k_msleep(20);
		}
	}

	wifi_connect_params = (struct wifi_connect_req_params) {
		.ssid = (const uint8_t *)wifi_credentials_ssid(&ssid_length),
		.ssid_length = ssid_length,
		.psk = (const uint8_t *)wifi_credentials_password(&password_length),
		.psk_length = password_length,
		.security = WIFI_SECURITY_TYPE_PSK,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.channel = WIFI_CHANNEL_ANY,
	};

	k_work_init_delayable(&wifi_connect_work, wifi_connect_work_handler);
	/* Let the ESP32 radio and network interface finish startup before the
	 * first association request; the driver handles later retries itself.
	 */
	(void)k_work_schedule(&wifi_connect_work, K_SECONDS(2));
	while (true) {
		usb_maintenance_poll();
		autoboot_mqtt_poll(atomic_get(&wifi_connected));
		k_msleep(20);
	}
}

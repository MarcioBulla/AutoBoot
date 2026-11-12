#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>

#include "certificate.h"
#include "mqtt_client.h"

LOG_MODULE_REGISTER(autoboot_mqtt, LOG_LEVEL_INF);

#define MQTT_TLS_TAG 31
#define MQTT_RX_SIZE 256
#define MQTT_TX_SIZE 256
#define MQTT_WS_TMP_SIZE 512
#define MQTT_COMMAND_SIZE 64
#define MQTT_HISTORY_SIZE 5
#define MQTT_PULSE_MS CONFIG_AUTOBOOT_PC_PULSE_MS
#define MQTT_RECONNECT_MS 5000
#define MQTT_CONNACK_TIMEOUT_MS 10000
#define MQTT_POLL_MS 250
#define MQTT_HISTORY_SETTING "autoboot_mqtt/command_ids"
#define MQTT_CONFIG_SETTING "autoboot_mqtt/config"
#define MQTT_HOST_SIZE 96

#define TOPIC_COMMAND "AutoBoot/command/+"
#define TOPIC_STATE "AutoBoot/state"
#define TOPIC_RESULT "AutoBoot/result"
#define TOPIC_AVAILABILITY "AutoBoot/availability"

static const struct gpio_dt_spec pc_start = GPIO_DT_SPEC_GET(DT_ALIAS(pc_start), gpios);
static const struct gpio_dt_spec pc_restart = GPIO_DT_SPEC_GET(DT_ALIAS(pc_restart), gpios);
static const struct gpio_dt_spec pc_pled = GPIO_DT_SPEC_GET(DT_ALIAS(pc_pled), gpios);

struct command_history {
	uint8_t ids[MQTT_HISTORY_SIZE][16];
	uint8_t next;
	uint8_t count;
};

struct mqtt_broker_config {
	char host[MQTT_HOST_SIZE];
	uint16_t port;
};

static struct command_history history;
static struct mqtt_broker_config broker_config = {
	.host = CONFIG_AUTOBOOT_MQTT_BROKER_HOST,
	.port = CONFIG_AUTOBOOT_MQTT_BROKER_PORT,
};
static struct mqtt_client client;
static struct sockaddr_in broker;
static sec_tag_t tls_tags[] = { MQTT_TLS_TAG };
static struct mqtt_topic subscription_topic;
static struct mqtt_subscription_list subscription;
static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_message;
static uint8_t rx_buffer[MQTT_RX_SIZE];
static uint8_t tx_buffer[MQTT_TX_SIZE];
static uint8_t ws_tmp_buffer[MQTT_WS_TMP_SIZE];
static uint8_t command_buffer[MQTT_COMMAND_SIZE];
static uint16_t message_id = 1;
static bool initialized;
static bool connected;
static bool connecting;
static bool abort_pending;
static bool history_ready;
static int64_t retry_at;
static int64_t connect_deadline;
static int64_t next_state_check;
static bool last_state;
static bool state_known;
static bool clock_synced;

static int synchronize_clock(void)
{
	struct sntp_time time;
	struct timespec realtime;
	int err = sntp_simple("pool.ntp.org", 5000U, &time);

	if (err != 0) {
		LOG_WRN("SNTP time sync failed (%d); TLS certificate dates cannot be trusted yet", err);
		return err;
	}
	realtime.tv_sec = (time_t)time.seconds;
	realtime.tv_nsec = (long)(((uint64_t)time.fraction * NSEC_PER_SEC) >> 32);
	err = sys_clock_settime(SYS_CLOCK_REALTIME, &realtime);
	if (err == 0) {
		clock_synced = true;
		LOG_INF("System time synchronized before TLS certificate validation");
	}
	return err;
}

static int publish_message(const char *topic, const uint8_t *payload,
			   size_t length, bool retained)
{
	struct mqtt_publish_param param = { 0 };

	param.message.topic.topic.utf8 = (uint8_t *)topic;
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	param.message.payload.data = (uint8_t *)payload;
	param.message.payload.len = length;
	param.message_id = message_id++;
	if (message_id == 0U) {
		message_id = 1U;
	}
	param.retain_flag = retained;
	return mqtt_publish(&client, &param);
}

static int pc_read_state(bool *on)
{
	/* GPIO_ACTIVE_LOW maps a physical low PLED signal to logical true. */
	int value = gpio_pin_get_dt(&pc_pled);

	if (value < 0) {
		return value;
	}
	*on = value > 0;
	return 0;
}

static bool publish_state(void)
{
	bool on;
	const uint8_t *value;
	int err = pc_read_state(&on);

	if (err != 0) {
		LOG_WRN("Unable to read PC state (%d)", err);
		state_known = false;
		return false;
	}
	value = (const uint8_t *)(on ? "ON" : "OFF");

	err = publish_message(TOPIC_STATE, value, on ? 2U : 3U, true);
	if (err != 0) {
		LOG_WRN("Unable to publish PC state");
		state_known = false;
		return false;
	}
	last_state = on;
	state_known = true;
	return true;
}

static int uuid_parse(const uint8_t *text, size_t length, uint8_t id[16])
{
	static const uint8_t hyphens[] = { 8, 13, 18, 23 };
	size_t source = 0;
	size_t output = 0;

	if (length != 36U) {
		return -EINVAL;
	}
	for (size_t i = 0; i < sizeof(hyphens); ++i) {
		if (text[hyphens[i]] != '-') {
			return -EINVAL;
		}
	}
	if (text[14] != '4' || (text[19] != '8' && text[19] != '9' &&
				text[19] != 'a' && text[19] != 'A' &&
				text[19] != 'b' && text[19] != 'B')) {
		return -EINVAL;
	}
	while (source < length) {
		uint8_t high;
		uint8_t low;
		uint8_t c = text[source++];

		if (c == '-') {
			continue;
		}
		if (c >= '0' && c <= '9') {
			high = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			high = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			high = c - 'A' + 10;
		} else {
			return -EINVAL;
		}
		c = text[source++];
		if (c >= '0' && c <= '9') {
			low = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			low = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			low = c - 'A' + 10;
		} else {
			return -EINVAL;
		}
		if (output >= 16U) {
			return -EINVAL;
		}
		id[output++] = (uint8_t)((high << 4) | low);
	}
	return output == 16U ? 0 : -EINVAL;
}

static int uuid_find(const uint8_t id[16])
{
	for (uint8_t i = 0; i < history.count; ++i) {
		if (memcmp(history.ids[i], id, 16U) == 0) {
			return 1;
		}
	}
	return 0;
}

static int uuid_persist_before_action(const uint8_t id[16])
{
	struct command_history next = history;
	int err;

	memcpy(next.ids[next.next], id, 16U);
	next.next = (uint8_t)((next.next + 1U) % MQTT_HISTORY_SIZE);
	if (next.count < MQTT_HISTORY_SIZE) {
		next.count++;
	}
	err = settings_save_one(MQTT_HISTORY_SETTING, &next, sizeof(next));
	if (err == 0) {
		history = next;
	}
	return err;
}

static void publish_result(const uint8_t uuid[36], const char *status)
{
	char payload[112];
	int length = snprintk(payload, sizeof(payload),
			      "{\"command_id\":\"%.*s\",\"status\":\"%s\"}",
			      36, uuid, status);

	if (length > 0 && (size_t)length < sizeof(payload)) {
		if (publish_message(TOPIC_RESULT, (const uint8_t *)payload,
				    (size_t)length, false) != 0) {
			LOG_WRN("Unable to publish command result");
		}
	}
}

static bool is_supported_command_topic(const uint8_t *topic, size_t topic_len)
{
	static const char prefix[] = "AutoBoot/command/";
	const uint8_t *name;
	size_t name_len;

	if (topic_len <= sizeof(prefix) - 1U ||
	    memcmp(topic, prefix, sizeof(prefix) - 1U) != 0) {
		return false;
	}
	name = topic + sizeof(prefix) - 1U;
	name_len = topic_len - (sizeof(prefix) - 1U);
	return (name_len == 5U && memcmp(name, "start", 5U) == 0) ||
	       (name_len == 7U && memcmp(name, "restart", 7U) == 0) ||
	       (name_len == 3U && memcmp(name, "get", 3U) == 0);
}

static bool find_command_uuid(const uint8_t *payload, size_t payload_len,
			      uint8_t uuid_text[36])
{
	static const char id_prefix[] = "\"command_id\":\"";
	uint8_t uuid[16];

	for (size_t i = 0; i + sizeof(id_prefix) - 1U <= payload_len; ++i) {
		const uint8_t *candidate;

		if (memcmp(payload + i, id_prefix, sizeof(id_prefix) - 1U) != 0) {
			continue;
		}
		candidate = payload + i + sizeof(id_prefix) - 1U;
		if ((size_t)(candidate - payload) + 37U <= payload_len &&
		    candidate[36] == '"' && uuid_parse(candidate, 36U, uuid) == 0) {
			memcpy(uuid_text, candidate, 36U);
			return true;
		}
		return false;
	}
	return false;
}

static void process_command(const uint8_t *topic, size_t topic_len,
			    const uint8_t *payload, size_t payload_len)
{
	static const char command_prefix[] = "AutoBoot/command/";
	static const char id_prefix[] = "\"command_id\":\"";
	const uint8_t *name;
	size_t name_len;
	const uint8_t *uuid_text = NULL;
	uint8_t uuid[16];
	bool is_get;
	int err;

	if (topic_len <= sizeof(command_prefix) - 1U ||
	    memcmp(topic, command_prefix, sizeof(command_prefix) - 1U) != 0) {
		return;
	}
	name = topic + sizeof(command_prefix) - 1U;
	name_len = topic_len - (sizeof(command_prefix) - 1U);
	if ((name_len == 5U && memcmp(name, "start", 5U) == 0) ||
	    (name_len == 7U && memcmp(name, "restart", 7U) == 0)) {
		is_get = false;
	} else if (name_len == 3U && memcmp(name, "get", 3U) == 0) {
		is_get = true;
	} else {
		return;
	}

	/* Accept only the specified compact JSON form and extract command_id. */
	for (size_t i = 0; i + sizeof(id_prefix) - 1U <= payload_len; ++i) {
		if (memcmp(payload + i, id_prefix, sizeof(id_prefix) - 1U) == 0) {
			uuid_text = payload + i + sizeof(id_prefix) - 1U;
			break;
		}
	}
	if (uuid_text == NULL || (size_t)(uuid_text - payload) + 37U > payload_len ||
	    uuid_text[36] != '"' || uuid_parse(uuid_text, 36U, uuid) != 0) {
		LOG_WRN("Rejected command with invalid UUIDv4");
		return;
	}
	if (!history_ready) {
		publish_result(uuid_text, "ERROR");
		return;
	}
	if (uuid_find(uuid)) {
		publish_result(uuid_text, "DUPLICATE");
		return;
	}
	if (is_get) {
		publish_result(uuid_text, publish_state() ? "COMPLETED" : "ERROR");
		return;
	}
	if (name_len == 5U) {
		bool on;

		err = pc_read_state(&on);
		if (err != 0) {
			LOG_ERR("Could not read PC state before start (%d)", err);
			publish_result(uuid_text, "ERROR");
			return;
		}
		if (on) {
			publish_result(uuid_text, "ALREADY_ON");
			return;
		}
	}
	/* Commit the UUID ring before the physical action to prevent replay after reboot. */
	if (uuid_persist_before_action(uuid) != 0) {
		LOG_ERR("Could not persist command UUID; action skipped");
		publish_result(uuid_text, "ERROR");
		return;
	}
	const struct gpio_dt_spec *output = name_len == 5U ? &pc_start : &pc_restart;
	err = gpio_pin_set_dt(output, 1);
	if (err != 0) {
		LOG_ERR("Could not assert PC control GPIO (%d)", err);
		publish_result(uuid_text, "ERROR");
		return;
	}
	k_msleep(MQTT_PULSE_MS);
	err = gpio_pin_set_dt(output, 0);
	if (err != 0) {
		LOG_ERR("Could not deassert PC control GPIO (%d)", err);
		publish_result(uuid_text, "ERROR");
		return;
	}
	publish_result(uuid_text, "COMPLETED");
}

static void mqtt_event_handler(struct mqtt_client *client_ctx,
			       const struct mqtt_evt *event)
{
	switch (event->type) {
	case MQTT_EVT_CONNACK:
		if (event->result == 0) {
			connected = true;
			connecting = false;
			LOG_INF("Connected to MQTT broker over WSS");
			(void)publish_message(TOPIC_AVAILABILITY,
					     (const uint8_t *)"online", 6U, true);
			(void)mqtt_subscribe(client_ctx, &subscription);
			publish_state();
			next_state_check = k_uptime_get() + MQTT_POLL_MS;
		} else {
			connected = false;
			connecting = false;
			LOG_ERR("MQTT CONNACK rejected (%d)", event->result);
		}
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *publish = &event->param.publish;
		size_t length = publish->message.payload.len;
		bool payload_complete = false;

		if (length < sizeof(command_buffer)) {
			int read = mqtt_readall_publish_payload(client_ctx, command_buffer, length);

			if (read == 0) {
				payload_complete = true;
				process_command(publish->message.topic.topic.utf8,
						publish->message.topic.topic.size,
						command_buffer, length);
			} else {
				LOG_WRN("Incomplete MQTT command payload; reconnecting");
				abort_pending = true;
			}
		} else {
			LOG_WRN("Ignoring oversized MQTT command: topic=%.*s payload=%u bytes max=%u",
				(int)publish->message.topic.topic.size,
				publish->message.topic.topic.utf8,
				(unsigned int)length,
				(unsigned int)(sizeof(command_buffer) - 1U));
			/* Preserve a bounded prefix for a possible correlated REJECTED result. */
			size_t prefix_len = MIN(length, sizeof(command_buffer));
			size_t discarded = 0U;
			uint8_t rejected_uuid[36];
			bool can_report = false;

			while (discarded < prefix_len) {
				size_t remaining = prefix_len - discarded;
				int read = mqtt_read_publish_payload_blocking(client_ctx,
								      command_buffer + discarded,
								      remaining);

				if (read <= 0 || (size_t)read > remaining) {
					LOG_WRN("Could not read oversized MQTT command prefix (%d)", read);
					abort_pending = true;
					break;
				}
				discarded += (size_t)read;
			}
			if (discarded == prefix_len &&
			    is_supported_command_topic(publish->message.topic.topic.utf8,
						       publish->message.topic.topic.size)) {
				can_report = find_command_uuid(command_buffer, prefix_len,
						       rejected_uuid);
			}

			/* Drain the remainder in small pieces before handling another packet. */
			while (!abort_pending && discarded < length) {
				size_t chunk = MIN(length - discarded, sizeof(command_buffer));
				int read = mqtt_read_publish_payload_blocking(client_ctx,
									      command_buffer, chunk);

				if (read <= 0 || (size_t)read > chunk) {
					LOG_WRN("Could not drain oversized MQTT payload (%d)", read);
					abort_pending = true;
					break;
				}
				discarded += (size_t)read;
			}
			payload_complete = discarded == length;
			if (payload_complete && can_report) {
				publish_result(rejected_uuid, "REJECTED");
			} else if (payload_complete) {
				LOG_WRN("Oversized command discarded without valid UUIDv4; no result sent");
			}
		}
		if (payload_complete && publish->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id = publish->message_id,
			};
			(void)mqtt_publish_qos1_ack(client_ctx, &ack);
		}
		break;
	}
	case MQTT_EVT_DISCONNECT:
		connected = false;
		connecting = false;
		LOG_WRN("MQTT disconnected (%d)", event->result);
		retry_at = k_uptime_get() + MQTT_RECONNECT_MS;
		break;
	case MQTT_EVT_PUBACK:
	case MQTT_EVT_SUBACK:
	case MQTT_EVT_PINGRESP:
		break;
	default:
		LOG_DBG("MQTT event %d result %d", event->type, event->result);
		break;
	}
}

static int load_command_history(void)
{
	ssize_t length = settings_load_one(MQTT_HISTORY_SETTING, &history, sizeof(history));

	/* settings_load_one() returns zero on some backends when this optional
	 * setting has never been written.  An empty UUID ring is valid. */
	if (length == 0 || length == -ENOENT) {
		memset(&history, 0, sizeof(history));
		return 0;
	}
	if (length < 0) {
		LOG_ERR("Could not read persisted command UUID ring (%d)", (int)length);
		return (int)length;
	}
	if (length != sizeof(history) || history.count > MQTT_HISTORY_SIZE ||
	    history.next >= MQTT_HISTORY_SIZE) {
		LOG_ERR("Invalid persisted command UUID ring; actions disabled");
		return -EBADMSG;
	}
	return 0;
}

static int mqtt_client_prepare(void)
{
	static uint8_t client_id[64];
	static const uint8_t will_payload[] = "offline";
	struct net_if *iface = net_if_get_wifi_sta();
	const struct net_linkaddr *link = iface != NULL ? net_if_get_link_addr(iface) : NULL;
	int id_len;

	if (client_id[0] == '\0') {
		if (link != NULL && link->len >= 6U) {
			id_len = snprintk((char *)client_id, sizeof(client_id),
					  CONFIG_AUTOBOOT_DEVICE_NAME "-%02X%02X%02X%02X%02X%02X",
					  link->addr[0], link->addr[1], link->addr[2],
					  link->addr[3], link->addr[4], link->addr[5]);
		} else {
			id_len = snprintk((char *)client_id, sizeof(client_id),
					  CONFIG_AUTOBOOT_DEVICE_NAME);
		}
		if (id_len < 0 || (size_t)id_len >= sizeof(client_id)) {
			return -EINVAL;
		}
	}

	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_event_handler;
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = strlen((const char *)client_id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.keepalive = 45U;
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.will_topic = &will_topic;
	client.will_message = &will_message;
	client.will_retain = 1U;
	client.transport.type = MQTT_TRANSPORT_SECURE_WEBSOCKET;
	client.transport.tls.config.peer_verify = TLS_PEER_VERIFY_REQUIRED;
	client.transport.tls.config.sec_tag_list = tls_tags;
	client.transport.tls.config.sec_tag_count = ARRAY_SIZE(tls_tags);
	client.transport.tls.config.hostname = broker_config.host;
	client.transport.websocket.config.host = broker_config.host;
	client.transport.websocket.config.url = "/mqtt";
	client.transport.websocket.config.tmp_buf = ws_tmp_buffer;
	client.transport.websocket.config.tmp_buf_len = sizeof(ws_tmp_buffer);
	client.transport.websocket.timeout = 5000;

	will_topic.topic.utf8 = (uint8_t *)TOPIC_AVAILABILITY;
	will_topic.topic.size = sizeof(TOPIC_AVAILABILITY) - 1U;
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	will_message.utf8 = (uint8_t *)will_payload;
	will_message.size = sizeof(will_payload) - 1U;
	return 0;
}

static bool hostname_valid(const char *host, size_t length)
{
	bool label_start = true;
	size_t label_length = 0;

	if (length == 0U || length >= MQTT_HOST_SIZE || host[0] == '.' ||
	    host[length - 1U] == '.' || host[length - 1U] == '-') {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		char c = host[i];

		if (c == '.') {
			if (label_start || label_length > 63U || host[i - 1U] == '-') {
				return false;
			}
			label_start = true;
			label_length = 0;
		} else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			   (c >= '0' && c <= '9') || c == '-') {
			if (label_start && c == '-') {
				return false;
			}
			label_start = false;
			label_length++;
		} else {
			return false;
		}
	}
	return label_length <= 63U;
}

int autoboot_mqtt_configure(const char *host, uint16_t port)
{
	struct mqtt_broker_config next = { 0 };
	size_t length;
	int err;

	if (host == NULL) {
		return -EINVAL;
	}
	length = strlen(host);
	if (!hostname_valid(host, length) || port == 0U) {
		return -EINVAL;
	}
	memcpy(next.host, host, length);
	next.port = port;
	err = settings_save_one(MQTT_CONFIG_SETTING, &next, sizeof(next));
	if (err != 0) {
		return err;
	}
	broker_config = next;
	broker.sin_port = htons(port);
	if (connected || connecting) {
		(void)mqtt_abort(&client);
		connected = false;
		connecting = false;
	}
	retry_at = 0;
	LOG_INF("Stored MQTT broker configuration (%s:%u)", host, port);
	return 0;
}

static void load_broker_config(void)
{
	struct mqtt_broker_config stored;
	ssize_t length = settings_load_one(MQTT_CONFIG_SETTING, &stored, sizeof(stored));

	if (length == sizeof(stored) && stored.port != 0U &&
	    memchr(stored.host, '\0', sizeof(stored.host)) != NULL &&
	    hostname_valid(stored.host, strlen(stored.host))) {
		broker_config = stored;
	}
	broker.sin_port = htons(broker_config.port);
}

int autoboot_mqtt_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&pc_start) || !gpio_is_ready_dt(&pc_restart) ||
	    !gpio_is_ready_dt(&pc_pled)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&pc_start, GPIO_OUTPUT_INACTIVE);
	if (err == 0) {
		err = gpio_pin_configure_dt(&pc_restart, GPIO_OUTPUT_INACTIVE);
	}
	if (err == 0) {
		err = gpio_pin_configure_dt(&pc_pled, GPIO_INPUT);
	}
	if (err != 0) {
		return err;
	}
	err = load_command_history();
	history_ready = (err == 0);
	if (!history_ready) {
		LOG_ERR("Command actions disabled until UUID history can be loaded");
	}
	err = certificate_register_mqtt_tls_credentials(MQTT_TLS_TAG);
	if (err != 0) {
		LOG_WRN("MQTT disabled until client and CA certificates are installed (%d)", err);
	} else {
		LOG_INF("MQTT mTLS credentials loaded");
	}
	load_broker_config();
	broker.sin_family = AF_INET;
	broker.sin_port = htons(broker_config.port);
	subscription_topic.topic.utf8 = (uint8_t *)TOPIC_COMMAND;
	subscription_topic.topic.size = sizeof(TOPIC_COMMAND) - 1U;
	subscription_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	subscription.list = &subscription_topic;
	subscription.list_count = 1U;
	subscription.message_id = 1U;
	(void)mqtt_client_prepare();
	initialized = true;
	return 0;
}

void autoboot_mqtt_poll(bool wifi_connected)
{
	int64_t now = k_uptime_get();
	int err;

	if (!initialized || !wifi_connected) {
		if (connected || connecting) {
			(void)mqtt_abort(&client);
			connected = false;
			connecting = false;
		}
		return;
	}
	if (!connected && !connecting && now >= retry_at) {
		struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
		struct zsock_addrinfo *result = NULL;

		if (!clock_synced && synchronize_clock() != 0) {
			retry_at = now + MQTT_RECONNECT_MS;
			return;
		}
		if (certificate_register_mqtt_tls_credentials(MQTT_TLS_TAG) != 0) {
			retry_at = now + MQTT_RECONNECT_MS;
			return;
		}
		err = zsock_getaddrinfo(broker_config.host, NULL, &hints, &result);
		if (err != 0 || result == NULL) {
			LOG_WRN("MQTT DNS lookup failed (%d)", err);
			retry_at = now + MQTT_RECONNECT_MS;
			return;
		}
		broker.sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
		zsock_freeaddrinfo(result);
		mqtt_client_prepare();
		err = mqtt_connect(&client);
		if (err != 0) {
			LOG_WRN("MQTT WSS connect failed (%d)", err);
			(void)mqtt_abort(&client);
			retry_at = now + MQTT_RECONNECT_MS;
			return;
		}
		connecting = true;
		connect_deadline = now + MQTT_CONNACK_TIMEOUT_MS;
		retry_at = now + MQTT_RECONNECT_MS;
		return;
	}
	if (connecting && now >= connect_deadline) {
		LOG_WRN("MQTT CONNACK timed out");
		(void)mqtt_abort(&client);
		connected = false;
		connecting = false;
		retry_at = now + MQTT_RECONNECT_MS;
		return;
	}
	if (!connected && !connecting) {
		return;
	}
	{
		struct zsock_pollfd descriptor = {
			.fd = client.transport.websocket.sock,
			.events = ZSOCK_POLLIN,
		};
		int ready = zsock_poll(&descriptor, 1U, 0);

		if (ready > 0 && (descriptor.revents & (ZSOCK_POLLIN | ZSOCK_POLLERR | ZSOCK_POLLHUP))) {
			err = mqtt_input(&client);
			if (abort_pending) {
				abort_pending = false;
				(void)mqtt_abort(&client);
				connected = false;
				connecting = false;
				retry_at = now + MQTT_RECONNECT_MS;
				return;
			}
			if (err != 0 && err != -EAGAIN) {
				LOG_WRN("MQTT input failed (%d)", err);
				(void)mqtt_abort(&client);
				connected = false;
				retry_at = now + MQTT_RECONNECT_MS;
				return;
			}
		}
	}
	if (!connected) {
		return;
	}
	err = mqtt_live(&client);
	if (err != 0 && err != -EAGAIN) {
		LOG_WRN("MQTT keepalive failed (%d)", err);
		(void)mqtt_abort(&client);
		connected = false;
		retry_at = now + MQTT_RECONNECT_MS;
		return;
	}
	if (now >= next_state_check) {
		bool state;

		if (pc_read_state(&state) != 0) {
			state_known = false;
		} else if (!state_known || state != last_state) {
			publish_state();
		}
		next_state_check = now + MQTT_POLL_MS;
	}
}

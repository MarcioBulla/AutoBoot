# AutoBoot

AutoBoot is an ESP32 project for remotely controlling a computer power interface. The goal is to power on, power off, restart, and read the computer state through Wi-Fi on the microcontroller and MQTT as the remote integration layer.

Current state: the firmware already includes the Wi-Fi configuration and reconnect flow. The MQTT broker container is operational with TLS, Let's Encrypt certificates, MQTT on `8883`, and MQTT over WebSocket on `443`. Firmware MQTT integration and physical power/reset control are still in progress.

## Structure

- `firmware/`: ESP32 firmware built with ESP-IDF
- `mqtt-broker/`: TLS-enabled MQTT broker container for a VPS

## Prerequisites

On the development machine:
- ESP-IDF installed and available through `source "$HOME/esp/esp-idf/export.sh"`
- ESP32 board connected through USB
- access to the board serial port, for example `/dev/ttyUSB0`

On the VPS:
- domain pointing to the public VPS IP address
- Docker with Compose or Podman with Compose
- port `80` available for the ACME HTTP challenge
- firewall opened for ports `80`, `443`, and `8883`
- if the VPS provider has an external firewall or security group, ingress rules for `80/tcp`, `443/tcp`, and `8883/tcp`

## How To Replicate

Clone the repository:

```bash
git clone <REPOSITORY_URL>
cd AutoBoot
```

Then follow this order:

1. prepare the MQTT broker on the VPS
2. configure and flash the ESP32 firmware
3. connect the ESP32 to Wi-Fi through the serial menu

## Prepare The VPS MQTT Broker

Enter the broker directory:

```bash
cd mqtt-broker
```

Edit `broker.env` with the real VPS values:

```env
MQTT_DOMAIN=mqtt.example.com
CERTBOT_EMAIL=admin@example.com
CERTBOT_ARGS=
CONTAINER_NAME=mqtt-broker
IMAGE_NAME=mqtt-broker-ws
ACME_HTTP_PORT=80
MQTT_TLS_PORT=8883
MQTT_WS_TLS_PORT=443
BASE_DIR=./runtime
MQTT_USER=mqtt_user
MQTT_PASSWORD=change_this_password
```

Main fields:
- `MQTT_DOMAIN`: public domain used by the TLS certificate
- `CERTBOT_EMAIL`: email used for Let's Encrypt registration
- `CERTBOT_ARGS`: extra Certbot flags, for example `--staging` while testing
- `ACME_HTTP_PORT`: keep this as `80`
- `MQTT_TLS_PORT`: keep this as `8883`
- `MQTT_WS_TLS_PORT`: keep this as `443`
- `BASE_DIR`: writable runtime directory used for Let's Encrypt data and Mosquitto persistence
- `MQTT_USER`: MQTT username
- `MQTT_PASSWORD`: MQTT password

The compose stack handles:
- the first Let's Encrypt certificate request
- certificate renewal in a dedicated Certbot container
- Mosquitto password file generation from `MQTT_USER` and `MQTT_PASSWORD`
- broker startup after the first certificate is issued

The stack runs with host networking. The services bind directly to the VPS network instead of using container port mapping. This avoids Podman NAT issues on restricted VPS environments.

For Podman-based deployments, run the stack with `sudo` when using ports `80` and `443`. Certbot needs port `80`, Mosquitto uses port `443`, and both are privileged ports.

Start with Docker:

```bash
docker compose --env-file broker.env up -d --build
```

Or start with Podman:

```bash
sudo podman compose --env-file broker.env up -d --build
```

Check the container:

```bash
docker compose --env-file broker.env ps
docker compose --env-file broker.env logs -f
```

With Podman:

```bash
sudo podman compose --env-file broker.env ps
sudo podman compose --env-file broker.env logs -f
```

The first startup may take longer because Certbot needs to issue the initial certificate before the broker can start.

When the broker is running, the logs should show Mosquitto listening on ports `8883` and `443`. You can confirm this on the VPS:

```bash
sudo ss -lntup | grep -E ':443|:8883'
```

Client endpoints:
- `mqtts://<MQTT_DOMAIN>:8883` for MQTT over TLS
- `wss://<MQTT_DOMAIN>:443` for MQTT over WebSocket TLS

Stop the broker when needed:

```bash
docker compose --env-file broker.env down
```

or:

```bash
sudo podman compose --env-file broker.env down
```

## Configure The Firmware

Enter the firmware directory:

```bash
cd firmware
```

Load the ESP-IDF environment:

```bash
source "$HOME/esp/esp-idf/export.sh"
```

Configure the project:

```bash
idf.py set-target esp32
idf.py menuconfig
```

In `menuconfig`, review mainly:
- `AutoBoot > Wi-Fi > Wi-Fi status LED GPIO`
- `AutoBoot > Wi-Fi > Wi-Fi status LED is active low`
- `AutoBoot > Wi-Fi > Manual Wi-Fi connection timeout`
- `AutoBoot > UART control > UART control task stack size`

## Flash The Microcontroller

Build:

```bash
idf.py build
```

Flash and open the serial monitor:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

If the board uses a different serial port, replace `/dev/ttyUSB0` with the correct port.

## Connect To Wi-Fi

With the serial monitor open:

1. Wait for the interactive UART menu.
2. Use the Wi-Fi configuration option.
3. Scan for available networks or enter the SSID manually.
4. Enter the network password.
5. Save the credentials.
6. Restart the board to validate automatic reconnect.

The credentials are stored in NVS. On the next boots, the ESP32 automatically tries to reconnect to the saved network.

## Tests

Integration tests run with `uv`, `pytest`, and `pytest-embedded` against a real ESP32 board:

```bash
cd firmware
source "$HOME/esp/esp-idf/export.sh"
WIFI_TEST_SSID="YOUR_SSID" WIFI_TEST_PASSWORD="YOUR_PASSWORD" UV_CACHE_DIR=.cache/uv \
uv run pytest --embedded-services esp,idf --port /dev/ttyUSB0 --target esp32 tests/pytest_wifi_console.py
```

## Security

Do not commit real production data:
- Wi-Fi passwords
- MQTT passwords or tokens
- private OTA URLs
- private certificates
- `broker.env` with real passwords

The `mqtt-broker/broker.env` file is kept in the repository to make replication straightforward. If you fill it with real values, review it before committing.

The firmware currently stores Wi-Fi credentials in protected NVS storage. Full device protection should also include Secure Boot, Flash Encryption, and eFuse configuration before production deployment.

## Goals

- [x] Serial menu for Wi-Fi configuration
- [x] Wi-Fi scan and manual SSID entry
- [x] Credential storage in NVS
- [x] Automatic reconnect on boot
- [x] Wi-Fi status indication
- [x] Integration tests for the Wi-Fi flow
- [x] TLS MQTT broker container with Certbot automation
- [ ] Firmware MQTT integration
- [ ] Physical power button control
- [ ] Physical reset button control
- [ ] PC power-state detection
- [ ] Final MQTT topics and payload definitions
- [ ] Secure Boot setup
- [ ] Flash Encryption setup
- [ ] eFuse configuration for production device locking
- [ ] Stronger protection strategy for credentials stored on the microcontroller
- [ ] OTA

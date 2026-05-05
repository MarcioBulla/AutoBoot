# AutoBoot

AutoBoot is an ESP32 project for remotely controlling a computer power interface. The goal is to power on, power off, restart, and read the computer state through Wi-Fi on the microcontroller and MQTT as the remote integration layer.

Current state: the firmware already includes the Wi-Fi configuration and reconnect flow. The MQTT broker container is operational with TLS, Let's Encrypt certificates for the public broker endpoint, and mutual TLS for device authentication. Firmware MQTT integration and physical power/reset control are still in progress.

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
git clone https://github.com/MarcioBulla/AutoBoot/
cd AutoBoot
```

### Setup Broker

```bash
cd mqtt-broker
```

Edit `broker.env`:

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
MQTT_CLIENT_CA_CN=AutoBoot Device CA
MQTT_TOPIC_PREFIX=devices
```

Main fields:
- `MQTT_DOMAIN`: public domain used by the TLS certificate
- `CERTBOT_EMAIL`: email used for Let's Encrypt registration
- `CERTBOT_ARGS`: extra Certbot flags, for example `--staging` while testing
- `ACME_HTTP_PORT`: keep this as `80`
- `MQTT_TLS_PORT`: keep this as `8883`
- `MQTT_WS_TLS_PORT`: keep this as `443`
- `BASE_DIR`: writable runtime directory used for Let's Encrypt data and Mosquitto persistence
- `MQTT_CLIENT_CA_CN`: common name for the private CA that signs device certificates
- `MQTT_TOPIC_PREFIX`: topic prefix used by the broker ACL pattern, default `devices`

Initialize the client CA and issue one certificate per device:

```bash
./init-client-ca.sh ./runtime "AutoBoot Device CA"
./issue-device-cert.sh ./runtime device-001
./issue-device-cert.sh ./runtime device-002
```

Start the broker:

```bash
sudo docker compose --env-file broker.env up -d --build
# or
sudo podman compose --env-file broker.env up -d --build
```

Check the services:

```bash
sudo docker compose --env-file broker.env ps
sudo docker compose --env-file broker.env logs -f
# or
sudo podman compose --env-file broker.env ps
sudo podman compose --env-file broker.env logs -f
```

### Control Device Credentials

- Each device gets its own directory under `runtime/pki/devices/<device-id>/`
- Each directory contains `<device-id>.key`, `<device-id>.crt`, and `ca.crt`
- Copy that device certificate set to that device

> [!CAUTION]
> Remove `<device-id>.key` from the VPS after provisioning.
> The only permanent place for a device private key should be the device itself.

#### Add Device Credentials

Create the certificate set for a new device:

```bash
./issue-device-cert.sh ./runtime device-003
```

Provision the generated files to the device. Adding a new device does not require restarting the broker stack.

To use the broker from a workstation, issue a separate client certificate for that machine:

```bash
./issue-device-cert.sh ./runtime operator-workstation
```

#### Revoke Device Credentials

```bash
./revoke-device-cert.sh ./runtime device-001

# Restart your stack
sudo docker compose --env-file broker.env restart broker
# or
sudo podman compose --env-file broker.env restart broker
```

### Tests Before Proceeding

> [!IMPORTANT]
> The first startup may take longer because Certbot needs to issue the initial certificate before Mosquitto can start.

#### Listener Check On The VPS

The broker uses host networking and expects ports `80`, `443`, and `8883` to be open on the VPS. When it is running, confirm the listeners on the VPS with:

```bash
sudo ss -lntup | grep -E ':443|:8883'
```

> [!NOTE]
> The `ss` check is intended to be run inside the VPS where the broker container is running.

#### Manual Broker Test On Both Endpoints

> [!IMPORTANT]
> Run the `8883` and `443` tests from a client machine outside the VPS.
> Complete the device credential provisioning above before running the client tests below.

Client endpoints:
- `mqtts://<MQTT_DOMAIN>:8883`
- `wss://<MQTT_DOMAIN>:443`

##### Test `8883`

> [!NOTE]
> Install the Mosquitto clients on your workstation before testing `8883`, so `mosquitto_pub` and `mosquitto_sub` are available.

For direct MQTT over TLS on `8883`:

```bash
mosquitto_sub \
  -h <MQTT_DOMAIN> \
  -p 8883 \
  --cafile /etc/ssl/certs/ca-certificates.crt \
  --cert runtime/pki/devices/<device-id>/<device-id>.crt \
  --key runtime/pki/devices/<device-id>/<device-id>.key \
  -t devices/<device-id>/status
```

```bash
mosquitto_pub \
  -h <MQTT_DOMAIN> \
  -p 8883 \
  --cafile /etc/ssl/certs/ca-certificates.crt \
  --cert runtime/pki/devices/<device-id>/<device-id>.crt \
  --key runtime/pki/devices/<device-id>/<device-id>.key \
  -t devices/<device-id>/status \
  -m "test over 8883"
```

Expected result:
- the `8883` subscriber receives the message published to `devices/<device-id>/status`
- the TLS handshake completes with the broker certificate for `<MQTT_DOMAIN>` and requires the client certificate

##### Test `443`

> [!NOTE]
> Install the Node.js MQTT CLI globally before testing `443` over `wss`:
> `npm install -g mqtt`

For secure WebSocket MQTT on `443`:

```bash
mqtt subscribe \
  -h <MQTT_DOMAIN> \
  -p 443 \
  -l wss \
  --ca /etc/ssl/certs/ca-certificates.crt \
  --cert runtime/pki/devices/<device-id>/<device-id>.crt \
  --key runtime/pki/devices/<device-id>/<device-id>.key \
  -i test-wss-sub \
  -t devices/<device-id>/ws-test \
  -v
```

```bash
mqtt publish \
  -h <MQTT_DOMAIN> \
  -p 443 \
  -l wss \
  --ca /etc/ssl/certs/ca-certificates.crt \
  --cert runtime/pki/devices/<device-id>/<device-id>.crt \
  --key runtime/pki/devices/<device-id>/<device-id>.key \
  -i test-wss-pub \
  -t devices/<device-id>/ws-test \
  -m "test over 443"
```

Expected result:
- the `443` `wss` subscriber receives the message published to `devices/<device-id>/ws-test`
- the TLS handshake completes with the broker certificate for `<MQTT_DOMAIN>` and requires the client certificate

### Configure Firmware

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

#### Build And Flash Firmware

Build:

```bash
idf.py build
```

Flash and open the serial monitor:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

If the board uses a different serial port, replace `/dev/ttyUSB0` with the correct port.

#### Connect To Wi-Fi

1. Access the serial monitor using `idf.py monitor`.
2. Wait for the ESP32 to boot.
3. Press `Enter` to access the UART menu.
4. Open the Wi-Fi configuration option.
5. Scan for available networks or enter the SSID manually.
6. Enter the network password.
7. Save the credentials.
8. Restart the board with `Ctrl+T`, then `Ctrl+R`, or press the DevKit reset button, to validate automatic reconnect.

> [!NOTE]
> After the credential is saved, the ESP32 keeps retrying indefinitely until it reconnects.

The credentials are stored in NVS. On the next boots, the ESP32 automatically tries to reconnect to the saved network.

#### Firmware Tests

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
- private OTA URLs
- private certificates
- `runtime/pki/`

The `mqtt-broker/broker.env` file is kept in the repository to make replication straightforward. The runtime PKI material stays under `mqtt-broker/runtime/` and is already ignored by Git.

The firmware currently stores Wi-Fi credentials in protected NVS storage. For device certificates, production protection should include Secure Boot, Flash Encryption, and eFuse configuration before deployment.

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

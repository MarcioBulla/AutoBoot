# AutoBoot

AutoBoot is an ESP32 project for remotely controlling a computer power interface. The goal is to power on, power off, restart, and read the computer state through Wi-Fi on the microcontroller and MQTT as the remote integration layer.

Current state: the firmware already includes the Wi-Fi configuration and reconnect flow. Firmware MQTT integration and physical power/reset control are still in progress. The reference secure MQTT broker stack lives in the `mqtt-broker/` submodule.

## Structure

- `firmware/`: ESP32 firmware built with ESP-IDF
- `mqtt-broker/`: Git submodule pointing to MQTT Trust Gateway, the reusable secure MQTT broker stack

## Prerequisites

On the development machine:
- ESP-IDF installed and available through `source "$HOME/esp/esp-idf/export.sh"`
- ESP32 board connected through USB
- access to the board serial port, for example `/dev/ttyUSB0`

For the broker VPS, use the `mqtt-broker/` submodule documentation.

## How To Replicate

Clone the repository:

```bash
git clone --recurse-submodules https://github.com/MarcioBulla/AutoBoot/
cd AutoBoot
```

If you cloned without submodules, initialize the broker stack with:

```bash
git submodule update --init --recursive
```

### MQTT Broker

The reusable broker stack is maintained as the `mqtt-broker/` submodule:

```text
https://github.com/MarcioBulla/MQTT-Trust-Gateway
```

To configure the VPS broker:

```bash
cd mqtt-broker
chmod +x setup-wizard.sh
sudo ./setup-wizard.sh
```

The broker documentation, Admin Web setup, step-ca flow, and device certificate instructions live in the submodule README.

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
- broker runtime data, device private keys, or production CA material

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

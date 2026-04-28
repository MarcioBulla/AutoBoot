# AutoBoot

Project to use an ESP32 as a remote power-on and control agent for a computer.

The core idea is to:
- power on or wake a computer remotely
- receive commands through MQTT
- update the ESP32 firmware remotely through OTA
- keep credentials and secrets out of logs and out of unnecessary exposure in the firmware

## Goal

The ESP32 acts as an embedded control point for remote computer boot automation. The device is expected to stay on the local network, maintain Wi-Fi connectivity, receive commands from an MQTT broker, and trigger the expected action on the hardware connected to the target computer.

Expected use cases include:
- powering on a PC remotely
- integrating power control with home automation or a private server
- updating the ESP32 firmware without physical access to the device
- reducing the risk of exposing SSIDs, passwords, tokens, or other secrets

## Current Structure

The firmware lives in `firmware/`.

Main files and areas:
- `firmware/main/main.c`: application entry point
- `firmware/main/connections/wifi.c`: Wi-Fi flow, serial console, and protected credential persistence
- `firmware/main/input_system/`: UART interactive input and helper utilities
- `firmware/tests/pytest_wifi_console.py`: Wi-Fi integration tests using `pytest-embedded`
- `firmware/pyproject.toml`: Python test environment managed with `uv`

## Wi-Fi Flow

The current firmware includes an interactive serial console for network setup.

Implemented capabilities:
- list Wi-Fi networks ranked by signal strength
- navigate networks in pages of 5 entries at a time
- select a network found in the scan
- enter the SSID manually when needed
- enter the password interactively
- save credentials locally for later reuse
- retry, change password, or reselect a network when a connection fails

The purpose of this flow is to avoid inconsistent states and make recovery possible without reflashing the device.

## MQTT

The intended project architecture uses MQTT as the main remote control channel.

Expected flow:
1. the ESP32 connects to Wi-Fi
2. it connects to the MQTT broker
3. it listens to authenticated command topics
4. it triggers the hardware responsible for powering on or waking the computer
5. it publishes status, errors, and acknowledgements when needed

Examples of future responsibilities in this layer:
- a topic for boot commands
- a topic for device status
- a topic for controlled remote reboot
- a topic for OTA update trigger

## OTA Through MQTT

The project is also designed to support remote firmware updates.

Expected architectural direction:
- the OTA process can be triggered through MQTT
- the MQTT message should not carry the firmware binary directly
- the firmware should receive only trusted metadata, such as a signed URL, version, and expected hash
- the downloaded binary should be validated before switching firmware

Important OTA concerns:
- verify artifact integrity
- ideally validate source authenticity
- avoid updating from arbitrary payloads published to the broker
- keep a rollback strategy whenever possible

## Security

The project is designed to avoid exposing operational secrets.

Current and intended guidelines:
- do not expose SSIDs or passwords in plaintext in unnecessary logs
- do not version secrets in the repository
- do not hardcode real tokens, real passwords, or real credentials in the firmware
- do not store sensitive real values in versioned tests
- store local credentials with application-side protection
- keep the flow ready to evolve to stronger mechanisms once the production provisioning model is defined

Sensitive items that must not be committed into versioned firmware:
- real user SSIDs
- real Wi-Fi passwords
- real MQTT tokens
- real broker usernames and passwords
- private OTA URLs
- API keys or operational secrets

## Testing

The current integration tests focus on the Wi-Fi console flow.

There are two main scenarios:
- `happy_path`: normal connection and status verification
- `recovery_paths`: wrong password, retry, password change, and network reselection

The tests use:
- `uv` for the Python environment
- `pytest`
- `pytest-embedded`
- a real board connected at `/dev/ttyUSB0`

Execution example:

```bash
cd firmware
source "$HOME/esp/esp-idf/export.sh"
WIFI_TEST_SSID="YOUR_SSID" WIFI_TEST_PASSWORD="YOUR_PASSWORD" UV_CACHE_DIR=.cache/uv \
uv run pytest --embedded-services esp,idf --port /dev/ttyUSB0 --target esp32 tests/pytest_wifi_console.py
```

## Current Status

The project already includes:
- an ESP-IDF firmware base
- an interactive serial console for Wi-Fi configuration
- protected application-side credential storage
- automated integration tests for the network setup flow

The natural next steps are:
- add the MQTT layer
- implement the actual hardware action used to power on the target computer
- add remote OTA with validation
- strengthen authentication and integrity policies for production use

# AutoBoot

AutoBoot is an ESP32-based controller for a computer power interface.

It is intended to:
- power on the computer
- power off the computer
- restart the computer
- detect whether the computer is on
- expose those actions and state through MQTT

## Hardware Model

The ESP32 is meant to be connected to the target computer front-panel signals.

Planned hardware layout:
- one optocoupler for the power button line
- one optocoupler for the reset button line
- one optocoupler for reading the power LED or another safe power-state signal

This keeps the ESP32 isolated from the motherboard signals.

## Firmware Scope

The current firmware focuses on a complete Wi-Fi connection system and the base needed before the MQTT and GPIO control layers are completed.

Current Wi-Fi features:
- interactive serial Wi-Fi menu over UART
- Wi-Fi scan with paged SSID selection
- manual SSID entry
- protected credential storage in NVS
- boot auto-connect using saved credentials
- continuous retry on boot until the saved network becomes available
- serial menu input taking priority over boot auto-connect
- recovery flow for wrong password, retry, and network reselection
- Wi-Fi status indication through a dedicated LED output
- automatic ESP32 restart if an established Wi-Fi connection is lost after IP acquisition
- embedded integration tests for setup, boot auto-connect, recovery paths, and disconnect handling

Main files:
- `firmware/main/wifi/wifi_core.c`: Wi-Fi stack init, connect flow, event handling, and disconnect policy
- `firmware/main/wifi/wifi_boot.c`: boot auto-connect task and retry loop
- `firmware/main/wifi/wifi_console.c`: UART Wi-Fi menu and credential flow
- `firmware/main/wifi/wifi_credentials.c`: protected credential persistence in NVS
- `firmware/main/control/`: UART control entrypoint and menu flow
- `firmware/main/input_system/`: UART input helpers
- `firmware/tests/pytest_wifi_console.py`: embedded Wi-Fi integration tests, including optional manual AP disruption mode
- `firmware/pyproject.toml`: Python test environment managed with `uv`

## MQTT Direction

MQTT is the planned remote control interface.

Expected commands:
- power on
- power off
- restart
- status request

Expected status data:
- whether the computer is on or off
- device state and relevant errors

## Security

The project should not expose operational secrets in firmware, logs, or versioned files.

Do not commit:
- real Wi-Fi SSIDs
- real Wi-Fi passwords
- real MQTT tokens
- real broker credentials
- real OTA URLs or other secrets

## Testing

Tests are run with `uv`, `pytest`, and `pytest-embedded` against a real ESP32 board.

Example:

```bash
cd firmware
source "$HOME/esp/esp-idf/export.sh"
WIFI_TEST_SSID="YOUR_SSID" WIFI_TEST_PASSWORD="YOUR_PASSWORD" UV_CACHE_DIR=.cache/uv \
uv run pytest --embedded-services esp,idf --port /dev/ttyUSB0 --target esp32 tests/pytest_wifi_console.py
```

## Next Steps

Next planned work:
- add MQTT communication
- add GPIO control for power and reset
- add PC power-state detection
- define MQTT topics and payloads
- add OTA once the update model is finalized

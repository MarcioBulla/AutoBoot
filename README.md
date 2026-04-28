# AutoBoot

AutoBoot is an ESP32-based device designed to control a computer remotely.

The actual goal of the project is simple:
- power on a computer
- power off a computer
- restart a computer
- detect whether the computer is currently on
- control those actions remotely through MQTT

The ESP32 is intended to be physically connected to the target computer, interacting with its front-panel control signals and a power-state indicator.

## Goal

The project aims to provide a small embedded controller that can:
- trigger the computer power button
- trigger the computer reset button
- support remote power-off behavior
- monitor whether the computer is powered on
- expose those capabilities through MQTT for remote use

This is not intended to be a general-purpose PC management agent. The main purpose is hardware-level remote control of the machine power state.

## Expected Hardware Design

The expected setup is:
- one output path to simulate the computer power button
- one output path to simulate the computer reset button
- one input path to detect whether the computer is on

The intended implementation uses optocouplers:
- one optocoupler for the power button line
- one optocoupler for the reset button line
- one optocoupler for reading the computer power LED or equivalent state signal

This keeps the ESP32 electrically isolated from the motherboard front-panel signals and reduces the risk of directly coupling the ESP32 GPIOs to the PC circuitry.

## Remote Control Through MQTT

MQTT is the main remote control interface planned for the device.

Expected MQTT responsibilities:
- receive a remote command to power on the PC
- receive a remote command to power off the PC
- receive a remote command to restart the PC
- publish whether the PC is currently on or off
- expose device state and errors when needed

Examples of future MQTT command topics:
- power on
- power off
- restart
- status request
- device health or heartbeat

## Power-State Detection

The project is intended to detect whether the computer is on by reading a hardware state signal, such as:
- the computer power LED through an optocoupler
- another safe signal that is only active when the machine is powered on

The current direction is to use the computer power LED path as the main state indicator.

## Current Firmware Scope

The firmware currently focuses on the network setup and test infrastructure needed before the MQTT and hardware control layers are finalized.

Current capabilities include:
- interactive Wi-Fi configuration through the serial console
- Wi-Fi scan and network selection
- protected local credential storage on the device side
- automated integration tests for the Wi-Fi configuration flow

Main files and areas:
- `firmware/main/main.c`: application entry point
- `firmware/main/connections/wifi.c`: Wi-Fi flow, serial console, and credential persistence
- `firmware/main/input_system/`: UART input helpers
- `firmware/tests/pytest_wifi_console.py`: integration tests using `pytest-embedded`
- `firmware/pyproject.toml`: Python test environment managed with `uv`

## Security

The project should avoid exposing operational secrets in firmware, logs, or versioned files.

Important rules:
- do not commit real SSIDs
- do not commit real Wi-Fi passwords
- do not commit real MQTT tokens
- do not commit real broker usernames or passwords
- do not hardcode real operational secrets in the firmware
- avoid printing sensitive values in logs

Sensitive values should stay outside the repository and outside versioned test defaults.

## Testing

The current automated tests focus on the Wi-Fi setup flow.

There are two main scenarios:
- `happy_path`: normal Wi-Fi connection and status verification
- `recovery_paths`: wrong password, retry, password change, and network reselection

The tests use:
- `uv`
- `pytest`
- `pytest-embedded`
- a real ESP32 board connected over serial

Example:

```bash
cd firmware
source "$HOME/esp/esp-idf/export.sh"
WIFI_TEST_SSID="YOUR_SSID" WIFI_TEST_PASSWORD="YOUR_PASSWORD" UV_CACHE_DIR=.cache/uv \
uv run pytest --embedded-services esp,idf --port /dev/ttyUSB0 --target esp32 tests/pytest_wifi_console.py
```

## Next Steps

The natural next steps for the project are:
- add MQTT communication
- add GPIO control for power and reset outputs
- add GPIO-based PC power-state detection
- define the MQTT command and status topics
- add OTA support when the remote update model is finalized

# AutoBoot

ESP32-S2 firmware and a USB provisioning tool for configuring Wi-Fi, device
certificates and an MQTT TLS endpoint. Hardware design files are included for KiCad.

## Project layout

- `firmware/`: Zephyr application, configuration and board overlay.
- `tool/autoboot_usb.py`: host utility for the USB CDC ACM maintenance interface.
- `tool/pyproject.toml`: Python dependency metadata.
- `board/`: KiCad schematic, PCB and project files.

## Build the firmware

Use a configured Zephyr workspace with `west`, an ESP32-S2 toolchain and the
Espressif radio blobs. This repository contains the application; it does not
include the Zephyr workspace or SDK.

From the repository root:

```sh
west build -b esp32s2_lolin_mini -d build firmware
```

Hardware pins and peripherals are defined in
`firmware/boards/esp32s2_lolin_mini.overlay`. Check this overlay against your
board wiring before flashing. Firmware configuration is in `firmware/prj.conf`
and application options are in `firmware/Kconfig`.

## USB provisioning

Install `uv` and connect the board's USB CDC ACM maintenance interface. Replace
`/dev/ttyACM1` below with its actual device path. The standalone script declares
its `pyserial` dependency, so `uv` can run it without creating a project virtual
environment.

View available commands:

```sh
uv run --script tool/autoboot_usb.py --help
```

Generate a certificate signing request and verify it:

```sh
uv run --script tool/autoboot_usb.py /dev/ttyACM1 csr > autoboot.csr
openssl req -in autoboot.csr -noout -verify
```

Have your certificate authority sign the CSR, then install the resulting client
certificate. Install the CA certificate used to validate the MQTT broker separately:

```sh
uv run --script tool/autoboot_usb.py /dev/ttyACM1 client
uv run --script tool/autoboot_usb.py /dev/ttyACM1 ca
```

Configure the MQTT TLS hostname and WebSocket port:

```sh
uv run --script tool/autoboot_usb.py /dev/ttyACM1 mqtt mqtt.example.com 443
```

Set Wi-Fi credentials and restart the device:

```sh
uv run --script tool/autoboot_usb.py /dev/ttyACM1 wifi 'YOUR_SSID' 'YOUR_PASSWORD'
```

Use the broker hostname expected by its TLS certificate. Obtain certificates
from your own CA or provisioning service; device-specific certificate material
is not distributed with this repository.

## Files kept local

The `.gitignore` excludes certificates, CSRs, private keys, environment files,
build outputs, Python environments, local tool state and KiCad backups/autosaves.
Keep the main KiCad project, schematic and PCB files under version control.

## MQTT topics

`AutoBoot` is the current `Device_ID` and forms the first segment of each topic.

| Topic | Use | QoS | Retained |
|---|---|---:|---|
| `AutoBoot/command/+` | Subscribes to `start`, `restart` and `get` | 1 | — |
| `AutoBoot/state` | Publishes `ON` or `OFF` | 1 | Yes |
| `AutoBoot/result` | Publishes the result with `command_id` | 1 | No |
| `AutoBoot/availability` | Publishes `online`; the LWT publishes `offline` | 1 | Yes |

Commands are published to `AutoBoot/command/start`,
`AutoBoot/command/restart` or `AutoBoot/command/get`. Each command payload
contains a new UUIDv4 in `command_id`, for example
`{"command_id":"<UUIDv4>"}`. Results include the same `command_id`. `COMPLETED`
confirms that the output pulse was issued; it does not confirm that the PC
finished starting or restarting.

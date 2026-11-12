#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///

"""Provision AutoBoot over its native USB CDC ACM port.

Examples:
  uv run --script tool/autoboot_usb.py /dev/ttyACM1 csr > autoboot.csr
  uv run --script tool/autoboot_usb.py /dev/ttyACM1 client device.crt
  uv run --script tool/autoboot_usb.py /dev/ttyACM1 ca gateway-ca.crt
  uv run --script tool/autoboot_usb.py /dev/ttyACM1 wifi test 12345678
  uv run --script tool/autoboot_usb.py /dev/ttyACM1 mqtt mqtt.example.com 18443
"""

import argparse
import sys
import time

import serial


class AutoBootUsb:
    def __init__(self, port):
        self.port = serial.Serial(port, baudrate=115200, timeout=0.1,
                                  write_timeout=5)
        # CDC ACM devices commonly wait for DTR before accepting serial data.
        self.port.dtr = True
        self.port.rts = True
        self.port.reset_input_buffer()
        self.port.reset_output_buffer()
        time.sleep(0.1)

    def close(self):
        self.port.close()

    def write(self, data):
        self.port.write(data)
        self.port.flush()

    def line(self, timeout=5):
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            byte = self.port.read(1)
            if not byte:
                continue
            if byte == b"\n":
                return bytes(data).rstrip(b"\r")
            data.extend(byte)
        raise TimeoutError("No response from AutoBoot USB port")

    def read_exactly(self, length):
        data = bytearray()
        deadline = time.monotonic() + 10
        while len(data) < length and time.monotonic() < deadline:
            data.extend(self.port.read(length - len(data)))
        if len(data) != length:
            raise TimeoutError("CSR transfer was incomplete")
        return bytes(data)

    def send_line(self, text):
        self.write(text.encode("ascii") + b"\n")


def send_certificate(usb, kind, filename):
    data = open(filename, "rb").read()
    usb.send_line(f"PUT {kind} {len(data)}")
    if usb.line() != b"READY":
        raise RuntimeError("AutoBoot refused the upload")
    usb.write(data)
    result = usb.line()
    if result != b"OK stored":
        raise RuntimeError(result.decode("ascii", "replace"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial_port", help="CDC ACM device, e.g. /dev/ttyACM1")
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("csr")
    for name in ("client", "ca"):
        command = subcommands.add_parser(name)
        command.add_argument("file", nargs="?",
                             default="device.crt" if name == "client" else "gateway-ca.crt")
    wifi = subcommands.add_parser("wifi")
    wifi.add_argument("ssid")
    wifi.add_argument("password")
    mqtt = subcommands.add_parser("mqtt")
    mqtt.add_argument("host", help="MQTT TLS hostname used for SNI")
    mqtt.add_argument("mqtt_port", type=int, help="MQTT WSS port")
    args = parser.parse_args()

    usb = AutoBootUsb(args.serial_port)
    try:
        if args.command == "csr":
            usb.send_line("CSR")
            header = usb.line().split()
            if len(header) != 3 or header[:2] != [b"BEGIN", b"CSR"]:
                raise RuntimeError("Invalid CSR response")
            sys.stdout.buffer.write(usb.read_exactly(int(header[2])))
            if usb.line() != b"END CSR":
                raise RuntimeError("CSR terminator missing")
        elif args.command == "wifi":
            usb.send_line(f"WIFI {args.ssid} {args.password}")
            if usb.line() != b"OK wifi-saved; reboot":
                raise RuntimeError("Wi-Fi credentials were rejected")
        elif args.command == "mqtt":
            usb.send_line(f"MQTT {args.host} {args.mqtt_port}")
            if usb.line() != b"OK mqtt-saved":
                raise RuntimeError("MQTT broker settings were rejected")
        else:
            send_certificate(usb, args.command.upper(), args.file)
    finally:
        usb.close()


if __name__ == "__main__":
    main()

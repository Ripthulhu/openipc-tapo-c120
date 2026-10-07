#!/usr/bin/env python3
"""Prepare a private OpenIPC U-Boot environment for a new C120."""

import argparse
import os
import re
import struct
import zlib
from pathlib import Path


ENV_SIZE = 0x10000
REQUIRED = {"bootargs", "bootcmd", "kernaddr", "kernsize", "rootaddr", "rootsize",
            "soc", "wlanssid", "wlanpass", "sensor", "ethaddr"}


def parse_env(image):
    if len(image) != ENV_SIZE or zlib.crc32(image[4:]) != struct.unpack("<I", image[:4])[0]:
        raise ValueError("template is not a valid 64 KiB U-Boot environment")
    entries = image[4:].split(b"\0\0", 1)[0].split(b"\0")
    variables = {}
    for entry in entries:
        if b"=" not in entry:
            raise ValueError("invalid environment entry")
        key, value = entry.split(b"=", 1)
        if not re.fullmatch(rb"[A-Za-z_][A-Za-z0-9_]*", key) or key in variables:
            raise ValueError("invalid or duplicate environment key")
        variables[key] = value
    if not {key.encode() for key in REQUIRED} <= variables.keys():
        raise ValueError("template lacks required boot or Wi-Fi variables")
    if variables[b"soc"] != b"ssc377":
        raise ValueError("template is not for SSC377")
    if not variables[b"wlanssid"] or not variables[b"wlanpass"]:
        raise ValueError("template has no Wi-Fi credentials")
    return variables


def build_env(variables):
    if any(not re.fullmatch(rb"[A-Za-z_][A-Za-z0-9_]*", key) or
           any(char in value for char in (b"\0", b"\r", b"\n")) for key, value in variables.items()):
        raise ValueError("invalid environment key or embedded line ending/NUL")
    data = b"\0".join(key + b"=" + value for key, value in variables.items()) + b"\0\0"
    if len(data) > ENV_SIZE - 4:
        raise ValueError("environment exceeds 64 KiB")
    body = data.ljust(ENV_SIZE - 4, b"\0")
    return struct.pack("<I", zlib.crc32(body)) + body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--sensor", required=True, choices=("sc430ai", "sc438hai"))
    args = parser.parse_args()

    mac = args.mac.lower()
    if not re.fullmatch(r"[0-9a-f]{2}(:[0-9a-f]{2}){5}", mac):
        parser.error("invalid MAC address")
    if int(mac[:2], 16) & 1:
        parser.error("MAC address must be unicast")

    variables = parse_env(args.template.read_bytes())
    if variables[b"ethaddr"].decode("ascii", "replace").lower() == mac:
        parser.error("new camera MAC matches template camera")
    variables[b"ethaddr"] = mac.encode("ascii")
    variables[b"sensor"] = args.sensor.encode("ascii")
    image = build_env(variables)
    if parse_env(image) != variables:
        raise RuntimeError("generated environment did not round-trip")

    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as output:
        output.write(image)
    print(f"Prepared {len(image)}-byte environment for {args.sensor} and {mac}; credentials not shown")


if __name__ == "__main__":
    main()

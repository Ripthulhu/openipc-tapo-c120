#!/usr/bin/env python3
"""Assemble a private 16 MiB C120 OpenIPC image; never writes a device."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

from c120_prepare_env import parse_env


def validate_rootfs(image, sensor=None):
    checker = Path(__file__).resolve().parents[3] / "general/scripts/check-rootfs-startup.py"
    with tempfile.TemporaryDirectory(prefix="c120-rootfs-check-") as directory:
        root = Path(directory) / "root"
        subprocess.run(["unsquashfs", "-no-progress", "-no-xattrs", "-d", str(root), str(image)],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, str(checker), str(root)], check=True)
        if sensor == "sc438hai":
            module = root / "lib/modules/5.10.61/sigmastar/sensor_sc438hai_mipi.ko"
            iq = root / "etc/sensors/sc438hai.bin"
            expected = ((module, "4c8311c930cbcc8596908bf5aee2917263f4e28c0e47dfaadaaa04ec48ff4141"),
                        (iq, "6613491c0fece80555abceee3948d0389cad588396e83ca25d84786531e1edf0"))
            for path, digest in expected:
                if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                    raise ValueError(f"unvalidated SC438HAI component: {path.relative_to(root)}")
            vendor = (root / "etc/init.d/S70vendor").read_text()
            loader = (root / "usr/bin/load_sigmastar").read_text()
            defaults = (root / "etc/default/majestic").read_text()
            if ("quarantine" in vendor.lower() or 'load_"$vendor" -i' not in vendor or
                    "sensor_sc438hai_mipi" not in loader or "SENSOR=sc438hai" not in defaults or
                    list(root.rglob("sc438hai_2lane.ko"))):
                raise ValueError("SC438HAI startup is quarantined or selects an incompatible driver")
            print("SC438HAI: cold-boot-tested source driver, IQ and startup selection OK")


def assemble(stage, mac, sensor):
    layout = (("u-boot-ssc377-nor.bin", 0, 0x40000),
              ("openipc-env.bin", 0x40000, 0x10000),
              ("uImage.ssc377", 0x50000, 0x200000),
              ("rootfs.squashfs.ssc377", 0x250000, 0x500000))
    parts = {name: (stage / name).read_bytes() for name, _, _ in layout}
    env = parse_env(parts["openipc-env.bin"])
    expected = {b"ethaddr": mac.encode(), b"sensor": sensor.encode(),
                b"kernaddr": b"0x50000", b"kernsize": b"0x200000",
                b"rootaddr": b"0x250000", b"rootsize": b"0x500000",
                b"rootmtd": b"5120k"}
    if any(env.get(key) != value for key, value in expected.items()):
        raise ValueError("environment does not match this camera and flash layout")
    kernel = parts["uImage.ssc377"]
    if len(kernel) < 64 or kernel[:4] != bytes.fromhex("27051956"):
        raise ValueError("invalid kernel uImage header")
    header = bytearray(kernel[:64])
    header[4:8] = bytes(4)
    if (zlib.crc32(header) != struct.unpack_from(">I", kernel, 4)[0] or
            len(kernel) != 64 + struct.unpack_from(">I", kernel, 12)[0] or
            zlib.crc32(kernel[64:]) != struct.unpack_from(">I", kernel, 24)[0]):
        raise ValueError("kernel length or CRC mismatch")
    rootfs = parts["rootfs.squashfs.ssc377"]
    if (len(rootfs) < 96 or rootfs[:4] != b"hsqs" or
            struct.unpack_from("<H", rootfs, 28)[0] != 4 or
            not 96 <= struct.unpack_from("<Q", rootfs, 40)[0] <= len(rootfs)):
        raise ValueError("invalid SquashFS rootfs")
    image = bytearray(b"\xff" * 0x1000000)
    for name, offset, capacity in layout:
        data = parts[name]
        if not 0 < len(data) <= capacity:
            raise ValueError(f"{name} does not fit its partition")
        image[offset:offset + len(data)] = data
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mac", required=True)
    parser.add_argument("--sensor", required=True, choices=("sc430ai", "sc438hai"))
    args = parser.parse_args()
    image = assemble(args.stage, args.mac.lower(), args.sensor)
    # Header/CRC checks cannot detect an unbootable /init or flattened Git links.
    validate_rootfs(args.stage / "rootfs.squashfs.ssc377", args.sensor)
    with args.output.open("xb") as output:
        output.write(image)
    print(f"{hashlib.sha256(image).hexdigest()}  {args.output.name}")


if __name__ == "__main__":
    main()

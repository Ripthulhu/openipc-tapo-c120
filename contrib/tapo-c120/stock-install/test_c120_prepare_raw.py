"""Run: python tools/test_c120_prepare_raw.py."""
from pathlib import Path
import struct
import tempfile
import zlib

from c120_prepare_env import REQUIRED, build_env
from c120_prepare_raw import assemble

with tempfile.TemporaryDirectory() as directory:
    stage = Path(directory)
    env = {key.encode(): b"test" for key in REQUIRED}
    env.update({b"soc": b"ssc377", b"ethaddr": b"60:15:6f:98:13:be", b"sensor": b"sc438hai",
                b"kernaddr": b"0x50000", b"kernsize": b"0x200000", b"rootaddr": b"0x250000",
                b"rootsize": b"0x500000", b"rootmtd": b"5120k"})
    (stage / "openipc-env.bin").write_bytes(build_env(env))
    (stage / "u-boot-ssc377-nor.bin").write_bytes(b"boot")
    payload = b"test kernel"
    kernel = bytearray(64)
    kernel[:4] = bytes.fromhex("27051956")
    struct.pack_into(">I", kernel, 12, len(payload))
    struct.pack_into(">I", kernel, 24, zlib.crc32(payload))
    struct.pack_into(">I", kernel, 4, zlib.crc32(kernel))
    (stage / "uImage.ssc377").write_bytes(kernel + payload)
    rootfs = bytearray(96)
    rootfs[:4] = b"hsqs"
    struct.pack_into("<H", rootfs, 28, 4)
    struct.pack_into("<Q", rootfs, 40, len(rootfs))
    (stage / "rootfs.squashfs.ssc377").write_bytes(rootfs)
    image = assemble(stage, "60:15:6f:98:13:be", "sc438hai")
    assert len(image) == 0x1000000 and image[:4] == b"boot"
    assert image[0x50000:0x50040] == kernel
    assert image[0x250000:0x250060] == rootfs
    assert image[0x750000:] == b"\xff" * 0x8b0000
    for mac, sensor in (("60:15:6f:98:13:bf", "sc438hai"), ("60:15:6f:98:13:be", "sc430ai")):
        try:
            assemble(stage, mac, sensor)
        except ValueError:
            pass
        else:
            raise AssertionError("wrong camera accepted")
    (stage / "uImage.ssc377").write_bytes(kernel + b"bad kernel!")
    try:
        assemble(stage, "60:15:6f:98:13:be", "sc438hai")
    except ValueError:
        pass
    else:
        raise AssertionError("corrupted kernel accepted")
print("PASS: layout, identity, padding and kernel integrity")

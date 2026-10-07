"""Maintainer Linux check: inspect both shipped SquashFS images, not just hashes."""
from pathlib import Path
import tempfile

from c120_install import HERE, PROFILES, check_kit, prepare_private_image
from c120_prepare_raw import validate_rootfs

kit = HERE / 'kit'
check_kit(kit)
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    for sensor in PROFILES:
        prepare_private_image(kit, work, '02:00:00:00:00:00', 'CHANGE_ME', 'CHANGE_ME', sensor)
        validate_rootfs(work / 'rootfs.squashfs.ssc377', sensor)
print('PASS: both packed images have executable startup files, expected sensor components and no owner password')

"""Maintainer Linux check: inspect both shipped SquashFS images, not just hashes."""
from pathlib import Path
import hashlib
import subprocess
import tempfile

from c120_install import HERE, PROFILES, check_kit, prepare_private_image
from c120_prepare_raw import validate_rootfs

kit = HERE / 'kit'
check_kit(kit)
components = []
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    for sensor in PROFILES:
        prepare_private_image(kit, work, '02:00:00:00:00:00', 'CHANGE_ME', 'CHANGE_ME', sensor)
        validate_rootfs(work / 'rootfs.squashfs.ssc377', sensor)
        def read(path):
            return subprocess.check_output(['unsquashfs', '-cat',
                                            str(work / 'rootfs.squashfs.ssc377'), path])

        components.append((hashlib.sha256(read('usr/bin/majestic')).hexdigest(),
                           read('var/www/.version').strip()))
        # Plugin installers rely on these upstream injection points.
        for path, anchors in {
            'var/www/cgi-bin/p/header.cgi': [b'href="stream-urls.cgi"'],
            'var/www/cgi-bin/live.cgi': [b'<script src="/a/preview-health.js"></script>'],
            'var/www/a/preview-page.js': [b'mj-audio-ctl', b'mj-talk-ctl'],
            'var/www/cgi-bin/stream-urls.cgi': [b'<dd>Toggle camera light.</dd>'],
        }.items():
            content = read(path)
            for anchor in anchors:
                assert anchor in content, (sensor, path, anchor)
assert len(set(components)) == 1, 'Both sensor images must ship matching Majestic and WebUI'
print('PASS: matching Majestic/WebUI and plugin integration anchors in both images')
print('PASS: both packed images have executable startup files, expected sensor components and no owner password')

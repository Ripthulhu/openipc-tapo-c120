#!/usr/bin/env python3
"""Maintainer-only Linux packaging; normal installs need no build tools."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from c120_install import HERE, FILES, PROFILES, digest, prepare_private_image
from c120_prepare_env import build_env, parse_env
from c120_prepare_raw import validate_rootfs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='complete input kit, including sc430ai/ images')
    parser.add_argument('--output', type=Path, required=True, help='new sanitized kit directory')
    parser.add_argument('--cc', default='arm-openipc-linux-musleabihf-gcc')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    scripts = {'rescue-init.sh': 'c120_rescue_init.sh',
               'recovery-flash.sh': 'c120_stock_recovery_flash.sh',
               'quiesce.sh': 'c120_rescue_quiesce.sh'}
    binaries = {'rescue-guard': 'c120_rescue_guard.c', 'mtdw-physical': 'mtd_offset_write.c'}
    for name in FILES:
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if name in scripts:
            target.write_text((HERE / scripts[name]).read_text(), encoding='ascii', newline='\n')
        elif name in binaries:
            subprocess.run([args.cc, '-Os', '-static', '-s', '-Wall', '-Wextra', '-Werror',
                            str(HERE / binaries[name]), '-o', str(target)], check=True)
        elif name == 'openipc-env.bin':
            env = parse_env((args.source / name).read_bytes())
            env.update({b'ethaddr': b'02:00:00:00:00:00', b'wlanssid': b'CHANGE_ME', b'wlanpass': b'CHANGE_ME'})
            target.write_bytes(build_env(env))
        else:
            shutil.copyfile(args.source / name, target)
    # Never publish header-only validation as evidence of a bootable rootfs.
    with tempfile.TemporaryDirectory() as work:
        for sensor in PROFILES:
            prepare_private_image(args.output, Path(work), '02:00:00:00:00:00', 'CHANGE_ME', 'CHANGE_ME', sensor)
            validate_rootfs(Path(work) / 'rootfs.squashfs.ssc377', sensor)
    manifest = {'version': 2, 'files': {name: digest(args.output / name) for name in FILES}}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', newline='\n')
    print('KIT_BUILT:', args.output)
    print('Review test results and pin MANIFEST_SHA256 to:', digest(args.output / 'manifest.json'))
    print('Packaging is not hardware validation. Retain each profile qualification status.')


if __name__ == '__main__':
    main()

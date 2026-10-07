#!/usr/bin/env python3
"""Package validated local artifacts without camera identities or credentials."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

from c120_install import ROOT, HERE, FILES, check_kit, digest
from c120_prepare_env import build_env, parse_env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='private validated build staging directory')
    parser.add_argument('--output', type=Path, required=True, help='new sanitized kit directory')
    parser.add_argument('--cc', default='arm-openipc-linux-musleabihf-gcc')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.source / 'sd-stage'
    scripts = {'rescue-init.sh': 'c120_rescue_init.sh',
               'recovery-flash.sh': 'c120_stock_recovery_flash.sh',
               'quiesce.sh': 'c120_rescue_quiesce.sh'}
    for name in FILES:
        if name in scripts:
            (args.output / name).write_text((HERE / scripts[name]).read_text(), encoding='ascii', newline='\n')
        elif name == 'rescue-guard':
            paths = [str(HERE / 'c120_rescue_guard.c'), str((args.output / name).resolve())]
            if os.name == 'nt':
                paths = [subprocess.check_output(['wsl', '--exec', 'wslpath', '-a', Path(p).as_posix()], text=True).strip() for p in paths]
            subprocess.run((['wsl', '--exec'] if os.name == 'nt' else []) +
                           [args.cc, '-Os', '-static', '-s', '-Wall', '-Wextra', '-Werror', paths[0], '-o', paths[1]], check=True)
        elif name == 'openipc-env.bin':
            env = parse_env((source / name).read_bytes())
            env.update({b'ethaddr': b'02:00:00:00:00:00', b'wlanssid': b'CHANGE_ME', b'wlanpass': b'CHANGE_ME'})
            (args.output / name).write_bytes(build_env(env))
        else:
            folder = args.source / 'sc438hai-source-build' if name in ('uImage.ssc377', 'rootfs.squashfs.ssc377') else source
            shutil.copyfile(folder / name, args.output / name)
    manifest = {'version': 1, 'sensor': 'sc438hai', 'stockVersion': '1.4.4 Build 260106 Rel.62350n',
                'stockMainSha256': digest(args.source / 'main_1.4.4'),
                'files': {name: digest(args.output / name) for name in FILES}}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    check_kit(args.output.resolve())
    print('KIT_READY:', args.output)


if __name__ == '__main__':
    main()

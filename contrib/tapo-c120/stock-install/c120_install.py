#!/usr/bin/env python3
"""Guided, narrowly supported stock C120 migration. Never accepts the OpenIPC EULA."""
import argparse
import base64
import getpass
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from urllib.request import urlopen

HERE = Path(__file__).resolve().parent
ROOT = HERE
from c120_prepare_env import build_env, parse_env
from c120_prepare_raw import assemble
from tapo_c120_factory_exec import run as factory_run
from tapo_client import TapoSession, encode_traversal_path, http_request, login_once, sha256_hex

FILES = ('busybox', 'libc.so', 'rescue-guard', 'rescue-runtime.tgz', 'mtdw-physical',
         'rescue-init.sh', 'recovery-flash.sh', 'quiesce.sh', 'openipc-env.bin',
         'u-boot-ssc377-nor.bin', 'uImage.ssc377', 'rootfs.squashfs.ssc377',
         'sc430ai/uImage.ssc377', 'sc430ai/rootfs.squashfs.ssc377')
BB = '/bin/busybox '
MANIFEST_SHA256 = 'bf8039c9e0c6c0f29dfbeb4ef58fa6d7fb2ff37e3b385cb9f5050d62a900b3a6'
PROFILES = {
    'sc438hai': {
        'version': '1.4.4 Build 260106 Rel.62350n',
        'main': 'ab1dd0d2ab8f2f29de17185dd0460fe4a28961008f6a433684f9eb4065f6803d',
        'module': 'drv_ms_cus_sc438hai_2lane',
    },
    'sc430ai': {
        'version': '1.4.1 Build 250910 Rel.58576n',
        'main': '6ea2fd02fa952dd998e433405c797a4b7686714f9eafabcd898e91dfa38adbef',
        'module': 'drv_ms_cus_sc430ai_MIPI_tp_ww',
    },
}


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def private_ip(value):
    address = ipaddress.IPv4Address(value)
    if not any(address in ipaddress.IPv4Network(net) for net in
               ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16')):
        raise ValueError('Use the camera\'s LAN IPv4 address')
    return str(address)


def mac_address(value):
    value = value.lower().replace('-', ':')
    if not re.fullmatch(r'[0-9a-f]{2}(:[0-9a-f]{2}){5}', value) or int(value[:2], 16) & 1 or value == '00:00:00:00:00:00':
        raise ValueError('Invalid camera MAC')
    return value


def checksum_file(folder, names, destination):
    text = ''.join(f'{digest(folder / name)}  {name}\n' for name in names)
    (folder / destination).write_text(text, encoding='ascii', newline='\n')


def check_kit(kit):
    if digest(kit / 'manifest.json') != MANIFEST_SHA256:
        raise ValueError('Unreviewed kit manifest; use the kit shipped with this installer')
    manifest = json.loads((kit / 'manifest.json').read_text())
    if (manifest.get('version') != 2 or
            set(manifest.get('files', {})) != set(FILES)):
        raise ValueError('Unsupported or incomplete installation kit')
    for name in FILES:
        path = kit / name
        if path.is_symlink() or digest(path) != manifest['files'][name]:
            raise ValueError('Kit checksum mismatch: ' + name)
        if name.endswith('.sh') and b'\r' in path.read_bytes():
            raise ValueError('Windows line endings in ' + name)
    # The pinned hashes bind the images to the maintainer's unpacked startup
    # checks. Installation itself needs neither Linux tools nor WSL.
    with tempfile.TemporaryDirectory(prefix='c120-kit-check-') as temp:
        template_mac = parse_env((kit / 'openipc-env.bin').read_bytes())[b'ethaddr'].decode()
        for sensor in PROFILES:
            prepare_private_image(kit, Path(temp), template_mac, 'CHANGE_ME', 'CHANGE_ME', sensor)
    return manifest


def select_profile(info, modules):
    if info.get('device_model') != 'C120' or info.get('hw_version') != '1.0':
        raise ValueError('Not a supported stock C120 v1.0')
    loaded = {line.split()[0] for line in modules.splitlines() if line.strip()}
    matches = [sensor for sensor, profile in PROFILES.items()
               if info.get('sw_version') == profile['version'] and profile['module'] in loaded]
    if len(matches) != 1:
        raise ValueError('Unsupported stock version/sensor pair; refusing to change it')
    return matches[0]


def stock(session, command):
    code, text = factory_run(session, command)
    if code:
        raise RuntimeError('Stock preparation failed; no flash launched: ' + text[-800:])
    return text


def stock_bytes(session, path):
    response = http_request(session.host, f'/stok={session.stok}/%2e%2e%2f{encode_traversal_path(path)}')
    if response['status'] != 200 or len(response['body']) > 65536:
        raise RuntimeError('Small preparation status file unavailable')
    return response['body']


def stock_file(session, path):
    return stock_bytes(session, path).rstrip(b'\0').decode('utf-8', 'replace')


def verify_stock_file(session, path, local):
    # Stock has no checksum applet. Read bounded chunks before trusting new tools.
    expected = local.read_bytes()
    size = int(stock(session, 'wc -c < ' + path).strip())
    if size != len(expected):
        raise RuntimeError('Stock transfer size mismatch: ' + local.name)
    for index, offset in enumerate(range(0, size, 32768)):
        stock(session, f'dd if={path} of=/tmp/oip bs=32768 skip={index} count=1')
        if stock_bytes(session, '/tmp/oip') != expected[offset:offset + 32768]:
            raise RuntimeError('Stock read-back mismatch: ' + local.name)


def remote(ssh, command, timeout=60):
    _, out, err = ssh.exec_command(command, timeout=timeout)
    text = out.read().decode('utf-8', 'replace')
    error = err.read().decode('utf-8', 'replace')
    if out.channel.recv_exit_status():
        raise RuntimeError('Recovery command failed: ' + (text + error)[-1200:])
    return text


def upload(ssh, local, name):
    # A failed transfer never replaces a usable file, and never launches anything.
    target = '/stage/' + name
    expected = digest(local)
    part = shlex.quote(target + '.part')
    inp, out, err = ssh.exec_command('set -eu; ' + BB + 'cat > ' + part +
                                    '; test "$(' + BB + 'sha256sum ' + part + ' | ' + BB +
                                    "cut -d ' ' -f 1)\" = " + expected + '; ' + BB + 'mv ' + part + ' ' + shlex.quote(target), timeout=120)
    with local.open('rb') as source:
        shutil.copyfileobj(source, inp, 32768)
    inp.flush()
    inp.channel.shutdown_write()
    out.read()
    if out.channel.recv_exit_status():
        raise RuntimeError('Upload failed: ' + err.read().decode(errors='replace')[-400:])
    actual = remote(ssh, BB + 'sha256sum ' + shlex.quote(target)).split()[0]
    if actual != expected:
        raise RuntimeError('Upload checksum mismatch: ' + name)


def backup(ssh, work):
    remote(ssh, '/stage/mtdw-physical c120-raw-read 0 0x1000000 /stage/raw-stock-quiesced.bin', 180)
    expected = remote(ssh, BB + 'sha256sum /stage/raw-stock-quiesced.bin').split()[0]
    _, out, err = ssh.exec_command(BB + 'cat /stage/raw-stock-quiesced.bin', timeout=180)
    path = work / 'raw-stock-quiesced.bin'
    with path.open('xb') as target:
        shutil.copyfileobj(out, target, 32768)
    if out.channel.recv_exit_status() or path.stat().st_size != 0x1000000 or digest(path) != expected:
        raise RuntimeError('Off-camera physical backup did not verify; refusing to flash')
    (work / 'stock-backup.sha256').write_text(expected + '  raw-stock-quiesced.bin\n')


def connect_recovery(session, work):
    import paramiko
    hostkey = stock_file(session, '/tmp/c120-rescue/hostkey.pub')
    match = re.search(r'^ssh-ed25519 ([A-Za-z0-9+/=]+)', hostkey, re.M)
    if not match:
        raise RuntimeError('Authenticated recovery host key unavailable')
    (work / 'recovery.hostkey').write_text(hostkey)
    ssh = paramiko.SSHClient()
    ssh.get_host_keys().add(f'[{session.host}]:2222', 'ssh-ed25519',
                           paramiko.Ed25519Key(data=base64.b64decode(match[1])))
    ssh.connect(session.host, port=2222, username='root', key_filename=str(work / 'recovery.key'),
                look_for_keys=False, allow_agent=False, timeout=15, banner_timeout=15)
    return ssh


def bootstrap(session, work, kit, bind, mac, port):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = Ed25519PrivateKey.generate()
    (work / 'recovery.key').write_bytes(key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.OpenSSH, serialization.NoEncryption()))
    os.chmod(work / 'recovery.key', 0o600)
    (work / 'recovery.pub').write_bytes(key.public_key().public_bytes(serialization.Encoding.OpenSSH,
        serialization.PublicFormat.OpenSSH) + b'\n')
    public = work / 'bootstrap'
    public.mkdir()
    shutil.copyfile(work / 'recovery.pub', public / 'recovery.pub')
    names = ('busybox', 'libc.so', 'rescue-guard', 'rescue-runtime.tgz', 'rescue-init.sh')
    for name in names:
        shutil.copyfile(kit / name, public / name)
    checksum_file(public, (*names, 'recovery.pub'), 'rescue.sha256')
    stage = '/tmp/mnt/harddisk_1/oi-' + secrets.token_hex(4)
    # Only fresh staging directories are ever executed. Never reuse an old SD installer.
    lines = ['#!/bin/sh', 'set -eu', "trap 'echo BOOTSTRAP_FAILED' 0",
             "grep -q '^/dev/mmcblk0 /tmp/mnt/harddisk_1 vfat rw[, ]' /proc/mounts",
             "test \"$(df /tmp/mnt/harddisk_1 | awk 'END {print $4}')\" -ge 65536",
             'mkdir ' + stage, 'cd ' + stage]
    for name in (*names, 'recovery.pub', 'rescue.sha256'):
        lines += [f'tftp -g -r {name} -l {name}.part {bind} {port}', f'mv {name}.part {name}']
    lines += ['trap - 0', 'echo BOOTSTRAP_READY']
    (public / 'b.sh').write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')
    log = (work / 'tftp.log').open('wb')
    server = subprocess.Popen([sys.executable, str(HERE / 'c120_tftp_serve.py'), '--root', str(public),
                               '--bind', bind, '--client', session.host, '--port', str(port)], stdout=log, stderr=log)
    try:
        time.sleep(1)
        if server.poll() is not None:
            raise RuntimeError(f'TFTP listener did not start; see tftp.log (UDP port {port})')
        stock(session, f'tftp -g -r b.sh -l /tmp/oif {bind} {port}')
        verify_stock_file(session, '/tmp/oif', public / 'b.sh')
        stock(session, 'sh /tmp/oif </dev/null >/tmp/oif.log 2>&1 &')
        for _ in range(60):
            time.sleep(2)
            status = stock_file(session, '/tmp/oif.log')
            if 'BOOTSTRAP_FAILED' in status:
                raise RuntimeError('SD staging failed; check SD free space and camera-to-PC UDP routing')
            if 'BOOTSTRAP_READY' in status:
                break
        else:
            raise RuntimeError('SD staging timed out; no installer will be launched')
        print('Checking recovery tools through the authenticated stock connection...', flush=True)
        for name in ('libc.so', 'busybox', 'rescue.sha256', 'rescue-init.sh'):
            verify_stock_file(session, stage + '/' + name, public / name)
            print('Verified recovery file: ' + name, flush=True)
        (work / 'recovery.json').write_text(json.dumps({'host': session.host, 'mac': mac, 'stage': stage}))
        stock(session, 'cd ' + stage + '&&chmod 755 libc.so busybox rescue-guard')
        stock(session, 'cd ' + stage + '&&sh rescue-init.sh .')
        return connect_recovery(session, work), stage
    finally:
        server.terminate()
        server.wait(timeout=10)
        log.close()


def prepare_private_image(kit, work, mac, ssid, password, sensor):
    if sensor not in PROFILES:
        raise ValueError('Unsupported sensor')
    if not 1 <= len(ssid.encode()) <= 32 or not (8 <= len(password.encode()) <= 63 or re.fullmatch('[0-9a-fA-F]{64}', password)):
        raise ValueError('Expected a 1-32 byte SSID and WPA2 password (8-63 bytes or 64 hex digits)')
    env = parse_env((kit / 'openipc-env.bin').read_bytes())
    env.update({b'ethaddr': mac.encode(), b'wlanssid': ssid.encode(), b'wlanpass': password.encode(),
                b'sensor': sensor.encode()})
    for name in ('u-boot-ssc377-nor.bin', 'uImage.ssc377', 'rootfs.squashfs.ssc377'):
        folder = kit / 'sc430ai' if sensor == 'sc430ai' and name != 'u-boot-ssc377-nor.bin' else kit
        shutil.copyfile(folder / name, work / name)
    (work / 'openipc-env.bin').write_bytes(build_env(env))
    (work / 'openipc-raw.bin').write_bytes(assemble(work, mac, sensor))
    os.chmod(work / 'openipc-raw.bin', 0o600)


def monitor(ssh, host, work):
    from paramiko import SSHException
    print('Flashing autonomously. Keep power and SD connected; do not run the installer again.', flush=True)
    deadline = time.monotonic() + 1200
    last = ''
    while time.monotonic() < deadline:
        try:
            text = remote(ssh, BB + 'tail -n 12 /stage/physical-flash.log', 15)
            (work / 'flash-tail.log').write_text(text)
            markers = [line for line in text.splitlines() if any(word in line for word in
                       ('WRITING_', 'VERIFY_OK', 'ROLLBACK_FAILED', 'RESTORING_'))]
            if markers and markers[-1] != last:
                last = markers[-1]; print(last, flush=True)
            if 'ROLLBACK_FAILED' in text:
                raise RuntimeError('Rollback failed. DO NOT REBOOT. Recovery SSH and watchdog remain active.')
            if 'STOCK_ROLLBACK_PHYSICAL_VERIFY_OK' in text:
                raise RuntimeError('Stock rollback verified; OpenIPC was NOT installed.')
        except (socket.timeout, EOFError, OSError, SSHException):
            break
        time.sleep(3)
    else:
        raise RuntimeError('Flash outcome unknown. Do not reboot or relaunch; inspect recovery and saved log.')
    print('Waiting for the OpenIPC setup page...', flush=True)
    for _ in range(60):
        try:
            with urlopen(f'http://{host}/setup.html', timeout=3) as response:
                page = response.read(131072).lower()
                if response.status == 200 and b'openipc' in page and b'password' in page:
                    print(f'OpenIPC booted: http://{host}/setup.html\nReview the licence and complete setup yourself.')
                    return
        except (OSError, TimeoutError):
            pass
        time.sleep(3)
    raise RuntimeError('No setup page yet. Do not reflash: check the DHCP lease for this camera MAC and retain its SD log.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host', nargs='?')
    parser.add_argument('--kit', type=Path, default=ROOT / 'kit')
    parser.add_argument('--check-kit', action='store_true', help='offline validation; never connects to a camera')
    parser.add_argument('--check-only', action='store_true', help='temporary recovery/readiness checks; never freezes or flashes')
    parser.add_argument('--resume', type=Path, help='private run folder; resume preparation only, never an attempted flash')
    parser.add_argument('--bind', help='PC LAN IPv4 address reachable by the camera for TFTP')
    parser.add_argument('--tftp-port', type=int, default=1069, help='unprivileged UDP listener port (default: 1069)')
    parser.add_argument('--allow-experimental-sc430ai', action='store_true',
                        help='acknowledge that SC430AI migration is dump-qualified, not live-flash-tested')
    args = parser.parse_args()
    if not 1024 <= args.tftp_port <= 65535:
        parser.error('--tftp-port must be between 1024 and 65535; no administrator/root needed')
    os.umask(0o077)
    kit = args.kit.resolve()
    check_kit(kit)
    if args.check_kit:
        print('KIT_OK: pinned SC438HAI and SC430AI images, layout and CRCs verified; no camera contacted')
        return
    host = private_ip(args.host or input('Stock camera IP: ').strip())
    username = input('Tapo account email: ').strip()
    password_hash = sha256_hex(getpass.getpass('Tapo account password: '))
    # One login only: never extend a stock lockout by retrying credentials.
    session = TapoSession(host, password_hash, login_once(host, username, password_hash))
    response = session.secure_request({'method': 'multipleRequest', 'params': {'requests': [
        {'method': 'getDeviceInfo', 'params': {'device_info': {'name': ['basic_info']}}}]}})
    result = response['result']['responses'][0]
    info = result.get('result', result)['device_info']['basic_info']
    if (info.get('device_model') != 'C120' or info.get('hw_version') != '1.0' or
            info.get('sw_version') not in {p['version'] for p in PROFILES.values()}):
        raise ValueError('Unsupported stock C120 hardware or firmware; refusing to change it')
    if info['sw_version'] == PROFILES['sc430ai']['version']:
        if not args.allow_experimental_sc430ai:
            raise ValueError('Older SC430AI migration is dump-qualified only. Review README, then use --allow-experimental-sc430ai')
        print('SC430AI: offline-qualified path; a complete UART-free migration has not been tested on hardware.', flush=True)
    # Stock HTTP file serving may use stat size, which is zero for /proc files.
    sensor = select_profile(info, stock(session, 'cat /proc/modules'))
    profile = PROFILES[sensor]
    mac = mac_address(info['mac'])
    work = args.resume.resolve() if args.resume else ROOT / 'private-runs' / (mac.replace(':', '') + '-' + time.strftime('%Y%m%d-%H%M%S'))
    if not args.resume:
        work.mkdir(parents=True, exist_ok=False)
    bind = private_ip(args.bind) if args.bind else None
    if not bind:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
            route.connect((host, 443)); bind = private_ip(route.getsockname()[0])
    print(f'C120 {mac} at {host}; private backup folder: {work}', flush=True)
    if args.resume:
        state = json.loads((work / 'recovery.json').read_text())
        if (state['host'] != host or state['mac'] != mac or
                not re.fullmatch('/tmp/mnt/harddisk_1/oi-[0-9a-f]{8}', state['stage'])):
            raise ValueError('Recovery state is for a different camera or staging directory')
        stage = state['stage']
        ssh = connect_recovery(session, work)
    else:
        ssh, stage = bootstrap(session, work, kit, bind, mac, args.tftp_port)
    frozen = launched = False
    try:
        remote(ssh, 'set -eu; for f in /var/run/stock-pids /var/run/watchdog.pid /stage/physical-flash.log /stage/raw-stock-quiesced.bin; do test ! -e "$f"; done')
        observed = mac_address(remote(ssh, BB + 'cat /sys/class/net/wlan0/address').strip())
        if observed != mac or remote(ssh, BB + 'sha256sum /host/bin/main').split()[0] != profile['main']:
            raise ValueError('Camera identity or exact stock application differs; refusing to continue')
        remote(ssh, BB + "grep -q '^" + profile['module'] + " ' /proc/modules")
        remote(ssh, BB + "grep -qx 'mtd15: 00fc0000 00001000 \"af\"' /proc/mtd")
        if args.check_only:
            print('READINESS_OK: exact stock application, sensor and recovery SSH verified. No flash writes. Reboot clears temporary SSH.')
            return
        ssid = input('OpenIPC Wi-Fi SSID (2.4 GHz): ')
        wifi = getpass.getpass('Wi-Fi password: ')
        prepare_private_image(kit, work, mac, ssid, wifi, sensor)
        for name in ('rescue-guard', 'mtdw-physical', 'recovery-flash.sh', 'quiesce.sh'):
            shutil.copyfile(kit / name, work / name)
            upload(ssh, work / name, name)
        upload(ssh, work / 'openipc-raw.bin', 'openipc-raw.bin')
        remote(ssh, BB + 'chmod 755 /stage/mtdw-physical /stage/rescue-guard')
        print('The next step pauses stock services to capture a consistent backup. Cancelling restores them.', flush=True)
        stock(session, stage + '/rescue-guard watchdog /tmp/c120-rescue/var/run/watchdog.pid $(pidof monitor)')
        frozen = True
        remote(ssh, BB + 'sh /stage/quiesce.sh --freeze', 60)
        backup(ssh, work)
        checksum_file(work, ('openipc-raw.bin', 'raw-stock-quiesced.bin', 'mtdw-physical', 'recovery-flash.sh'), 'physical-flash.sha256')
        upload(ssh, work / 'physical-flash.sha256', 'physical-flash.sha256')
        print(remote(ssh, BB + 'sh /stage/recovery-flash.sh --preflight ' + mac + ' ' + sensor, 180), flush=True)
        if input(f'Backup verified. Type FLASH {mac} to replace stock firmware: ').strip() != 'FLASH ' + mac:
            print('Cancelled. Restoring stock services; no flash writes.')
            return
        # An ambiguous SSH outcome must never cause a second launch or a stock-service resume.
        launched = True
        remote(ssh, "trap '' HUP; " + BB + 'setsid ' + BB + 'sh /stage/recovery-flash.sh --flash --yes-i-understand ' + mac + ' ' + sensor +
               ' </dev/null >/stage/physical-flash.log 2>&1 &', 15)
        monitor(ssh, host, work)
    finally:
        if frozen and not launched:
            print(remote(ssh, BB + 'sh /stage/quiesce.sh --resume'), flush=True)
        ssh.close()
    if launched:
        print('Keep the private backup folder. A completed write is not a substitute for checking video after setup.')


if __name__ == '__main__':
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        sys.exit('Stopped: ' + str(error) + '\nNo automatic retries. If flashing had started, keep power and SD connected.')

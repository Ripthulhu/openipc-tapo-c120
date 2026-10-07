#!/usr/bin/env python3
"""Run without a camera: python test_c120_install.py."""
from contextlib import ExitStack
from itertools import product
import io
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

import c120_install as app
from c120_prepare_env import build_env, parse_env
from tapo_c120_factory_exec import command_payload


def rejected(call):
    try:
        call()
    except (ValueError, RuntimeError):
        return
    raise AssertionError('Unsafe input accepted')


for value in ('127.0.0.1', '8.8.8.8', '::1', '192.168.1.1;reboot'):
    rejected(lambda: app.private_ip(value))
for value in ('ff:ff:ff:ff:ff:ff', '00:00:00:00:00:00', 'bad'):
    rejected(lambda: app.mac_address(value))
for value in (b'a\0b', b'a\nb', b'a\rb'):
    rejected(lambda: build_env({b'wlanssid': value}))
assert app.mac_address('60-15-6F-98-16-5C') == '60:15:6f:98:16:5c'
# A daemon must not keep the stock command result file open and overwrite its status.
assert '> "$ROOT/dropbear.log" 2>&1' in (app.HERE / 'c120_rescue_init.sh').read_text()
# The shipped snapshot must match the reviewed scripts and contain no identity.
kit = app.HERE / 'kit'
manifest = json.loads((kit / 'manifest.json').read_text())
assert set(manifest['files']) == set(app.FILES)
for name, sha in manifest['files'].items():
    assert app.digest(kit / name) == sha, name
for bundled, source in {'rescue-init.sh': 'c120_rescue_init.sh',
                        'recovery-flash.sh': 'c120_stock_recovery_flash.sh',
                        'quiesce.sh': 'c120_rescue_quiesce.sh'}.items():
    assert (kit / bundled).read_bytes() == (app.HERE / source).read_text().encode(), bundled
env = parse_env((kit / 'openipc-env.bin').read_bytes())
assert env[b'ethaddr'] == b'02:00:00:00:00:00'
assert env[b'wlanssid'] == env[b'wlanpass'] == b'CHANGE_ME'
assert app.digest(kit / 'manifest.json') == app.MANIFEST_SHA256
for sensor, profile in app.PROFILES.items():
    info = {'device_model': 'C120', 'hw_version': '1.0', 'sw_version': profile['version']}
    assert app.select_profile(info, profile['module'] + ' 100 0') == sensor
    for bad in ({**info, 'device_model': 'C200'}, {**info, 'sw_version': 'new unknown firmware'},
                {**info, 'hw_version': '2.0'}):
        rejected(lambda: app.select_profile(bad, profile['module']))
    rejected(lambda: app.select_profile(info, 'wrong_sensor 100 0'))
stage = '/tmp/mnt/harddisk_1/oi-12345678'
for command in ('cd ' + stage + '&&chmod 755 libc.so busybox rescue-guard',
                stage + '/rescue-guard watchdog /tmp/c120-rescue/var/run/watchdog.pid $(pidof monitor)',
                'dd if=' + stage + '/rescue-init.sh of=/tmp/oip bs=32768 skip=23 count=1'):
    assert len(command_payload(command, '/tmp/o12345678')) <= 190


class Channel:
    def recv_exit_status(self):
        return 0


class Stream(io.BytesIO):
    channel = Channel()


class SSH:
    def exec_command(self, *args, **kwargs):
        return None, Stream(b'truncated'), Stream()

    def close(self):
        pass


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    # Updating an image and its manifest together still must not bypass review.
    badkit = root / 'badkit'
    badkit.mkdir()
    (badkit / 'manifest.json').write_text(json.dumps({'version': 2, 'files': {}}))
    rejected(lambda: app.check_kit(badkit))
    image_dir = root / 'image'
    image_dir.mkdir()
    for sensor in app.PROFILES:
        app.prepare_private_image(kit, image_dir, '02:11:22:33:44:55', 'example-wifi', 'example-password', sensor)
        configured = parse_env((image_dir / 'openipc-env.bin').read_bytes())
        assert configured[b'sensor'].decode() == sensor and configured[b'ethaddr'] == b'02:11:22:33:44:55'
        expected = kit / 'sc430ai' if sensor == 'sc430ai' else kit
        assert app.digest(image_dir / 'rootfs.squashfs.ssc377') == app.digest(expected / 'rootfs.squashfs.ssc377')
    with patch.object(app.subprocess, 'run', side_effect=AssertionError('Host tool used')), \
            patch.object(app.subprocess, 'check_output', side_effect=AssertionError('Host tool used')):
        app.check_kit(kit)
    local = root / 'readback'
    local.write_bytes(b'\0' * 32768 + b'final\0')
    with patch.object(app, 'stock', return_value=str(local.stat().st_size)), \
            patch.object(app, 'stock_bytes', side_effect=[b'\0' * 32768, b'final\0']):
        app.verify_stock_file(None, '/tmp/example', local)
    with patch.object(app, 'stock', return_value=str(local.stat().st_size)), \
            patch.object(app, 'stock_bytes', return_value=b'bad'):
        rejected(lambda: app.verify_stock_file(None, '/tmp/example', local))
    with patch.object(app, 'remote', side_effect=['', '0' * 64]):
        rejected(lambda: app.backup(SSH(), root))

    # Failure or cancellation after the freeze must resume stock, never launch.
    mac = '60:15:6f:98:16:5c'
    for sensor, phase in product(app.PROFILES, ('backup', 'preflight', 'cancel', 'resume-wrong-camera',
                                               'resume-after-flash', 'unacknowledged', 'wrong-main', 'launch-disconnect')):
        profile = app.PROFILES[sensor]
        info = {'device_model': 'C120', 'hw_version': '1.0', 'sw_version': profile['version'], 'mac': mac}
        if phase == 'unacknowledged' and sensor != 'sc430ai':
            continue
        calls = []
        arguments = ['install', '192.168.1.10', '--bind', '192.168.1.20']
        if sensor == 'sc430ai' and phase != 'unacknowledged':
            arguments += ['--allow-experimental-sc430ai']
        if phase.startswith('resume-'):
            work = root / (sensor + phase)
            work.mkdir()
            (work / 'recovery.json').write_text(json.dumps({'host': '192.168.1.10', 'stage': stage,
                'mac': '60:15:6f:98:16:5d' if phase == 'resume-wrong-camera' else mac}))
            arguments += ['--resume', str(work)]

        def remote(ssh, command, *args):
            calls.append(command)
            if command.endswith('/address'):
                return mac
            if command.endswith('/host/bin/main'):
                return '0' * 64 if phase == 'wrong-main' else profile['main']
            if '--preflight' in command and phase == 'preflight':
                raise RuntimeError('injected preflight failure')
            if '/stage/physical-flash.log' in command and phase == 'resume-after-flash':
                raise RuntimeError('an attempted flash exists')
            if 'setsid' in command and phase == 'launch-disconnect':
                raise RuntimeError('ambiguous launch response')
            return ''

        with ExitStack() as stack:
            for name, value in {'ROOT': root / (sensor + phase), 'check_kit': lambda _: manifest,
                                'login_once': lambda *a: {}, 'bootstrap': lambda *a: (SSH(), stage),
                                'connect_recovery': lambda *a: SSH(),
                                'prepare_private_image': lambda *a: None,
                                'upload': lambda *a: None, 'stock': lambda *a: profile['module'] + ' 0 0',
                                'checksum_file': lambda *a: None, 'remote': remote}.items():
                stack.enter_context(patch.object(app, name, value))
            stack.enter_context(patch.object(app.shutil, 'copyfile'))
            stack.enter_context(patch.object(app.sys, 'argv', arguments))
            stack.enter_context(patch('builtins.input', side_effect=['test@example.invalid', 'testssid',
                'FLASH ' + mac if phase == 'launch-disconnect' else 'CANCEL']))
            stack.enter_context(patch.object(app.getpass, 'getpass', return_value='testpassword'))
            stack.enter_context(patch.object(app, 'TapoSession')).return_value.secure_request.return_value = {
                'result': {'responses': [{'result': {'device_info': {'basic_info': info}}}]}}
            stack.enter_context(patch.object(app, 'backup', side_effect=RuntimeError('injected backup failure') if phase == 'backup' else None))
            if phase == 'cancel':
                app.main()
            else:
                rejected(app.main)
        assert any('--resume' in command for command in calls) == (phase in ('backup', 'preflight', 'cancel')), calls
        assert sum('--flash' in command for command in calls) == (1 if phase == 'launch-disconnect' else 0), calls
print('PASS: both profiles, kit pinning, portable validation, identity guards and fail-closed flash sequencing')

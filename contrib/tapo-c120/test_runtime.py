"""Run on Linux with gcc and Haserl. GPIOs/services are simulated under a temp directory."""
import os
import fcntl
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
from pathlib import Path
import signal
import shutil
import subprocess
import tarfile
import tempfile
import threading
import time
from urllib.parse import urlencode


BASE = Path(__file__).resolve().parent


def test_payloads():
    ap = BASE / 'ap-recovery-plugin'
    runtime = BASE / 'runtime-overlay'
    assert not (ap / 'files/usr/bin/c120-eventd').exists()
    assert not (runtime / 'usr/bin/c120-eventd').exists()
    assert not (ap / 'files/etc/init.d/S45c120-ap-button').exists()
    assert (ap / 'files/usr/bin/c120-button-apd').read_bytes() == (runtime / 'usr/bin/c120-button-apd').read_bytes()
    with tempfile.TemporaryDirectory(prefix='c120-package-') as temp:
        root = Path(temp)
        # Host compilation exercises packaging only, not the target ABI.
        compiler = root / 'host-package-cc'
        write(compiler, '#!/bin/sh\nif [ "$1" = -dumpmachine ]; then\n'
              'echo arm-openipc-linux-musleabihf\nelse\nexec "${HOSTCC:-gcc}" "$@"\nfi\n')
        compiler.chmod(0o755)
        out = root / 'build with spaces'
        env = dict(os.environ, CC=str(compiler))
        command = ['sh', str(BASE / 'build-plugin.sh'), str(out)]
        rejected = subprocess.run(command, env=dict(env, CC=os.environ.get('HOSTCC', 'gcc')),
                                  capture_output=True, text=True)
        assert rejected.returncode != 0 and 'ARM hard-float musl' in rejected.stderr
        for _ in range(2):
            subprocess.run(command, env=env, check=True, capture_output=True)
            binary = (out / 'c120-eventd').read_bytes()
            for name, prefix in (('openipc-c120-ap-recovery-plugin', 'files'),
                                 ('openipc-c120-runtime', 'runtime-overlay')):
                with tarfile.open(out / f'{name}.tgz') as archive:
                    packaged = archive.extractfile(f'{name}/{prefix}/usr/bin/c120-eventd').read()
                    digest, path = archive.extractfile(f'{name}/eventd.sha256').read().decode().split()
                    assert packaged == binary
                    assert path == f'{prefix}/usr/bin/c120-eventd'
                    assert hashlib.sha256(packaged).hexdigest() == digest
                    init = archive.extractfile(f'{name}/{prefix}/etc/init.d/S45c120-ap-button').read()
                    assert init == (runtime / 'etc/init.d/S45c120-ap-button').read_bytes()
                    installer = 'install.sh' if prefix == 'files' else 'install-runtime.sh'
                    assert archive.extractfile(f'{name}/{installer}').read() == (ap / installer if prefix == 'files' else BASE / installer).read_bytes()
                    assert archive.getmember(f'{name}/{prefix}/usr/bin/c120-eventd').mode & 0o111
                    if prefix == 'files':
                        assert archive.extractfile(f'{name}/hostapd-overlay.tgz').read() == (ap / 'hostapd-overlay.tgz').read_bytes()
                    else:
                        for asset in ('dashboard-luminance.sed', 'dashboard-memory.html', 'floodlight-url.html', 'live-audio.html'):
                            assert archive.extractfile(f'{name}/{asset}').read() == (BASE / asset).read_bytes()
                        for asset in ('var/www/a/c120-memory.js', 'var/www/cgi-bin/c120-memory.cgi'):
                            assert archive.extractfile(f'{name}/{prefix}/{asset}').read() == (runtime / asset).read_bytes()
            assert not list(out.glob('.packages.*')), 'packaging left temporary files'
        assert not (ap / 'files/usr/bin/c120-eventd').exists()
        assert not (runtime / 'usr/bin/c120-eventd').exists()
    plugin = BASE / 'ai-plugin'
    scripts = [path for directory in (BASE, ap, plugin, BASE / 'ai-probe')
               for path in directory.glob('*.sh')]
    for overlay in (ap / 'files', runtime, plugin / 'files'):
        scripts.extend(path for path in overlay.rglob('*') if path.is_file())
    for path in scripts:
        with path.open('rb') as source:
            shell = source.read(64).startswith(b'#!/bin/sh\n')
        if shell:
            subprocess.run(['sh', '-n', str(path)], check=True)
    # AI backups contain notification secrets; exercise the actual installer umask.
    with tempfile.TemporaryDirectory() as temp:
        backup = Path(temp) / 'backup.tar'
        prefix = (plugin / 'install.sh').read_text().split('src=')[0]
        subprocess.run(['sh', '-c', 'umask 000\n' + prefix + '\n: > "$1"',
                        'test-backup', str(backup)], check=True)
        assert backup.stat().st_mode & 0o077 == 0
    print('PASS payloads: one build, self-contained recovery/runtime packages, checksums, no source binaries')


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data)


def wait_for(check, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.02)
    raise AssertionError("condition not reached before timeout")


def test_daemon(root):
    root.mkdir(parents=True)
    metrics = {"night": "1", "count": 100, "received": 100, "error": False, "stall": False, "requests": 0}

    class MetricsHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            metrics["requests"] += 1
            if metrics["stall"]:
                time.sleep(0.3)
            value = metrics["night"] if self.path == "/metrics/night" else metrics["count"]
            key = "night_enabled" if self.path == "/metrics/night" else "md_rects_acc_total"
            body = f"# Native metric\n{key} {value}\n".encode()
            if self.path == "/metrics/motion":
                body = f'md_rects_recv_total {metrics["received"]}\n'.encode() + body
            self.send_response(503 if metrics["error"] else 200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), MetricsHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    binary = root / "c120-eventd"
    subprocess.run([
        os.environ.get("HOSTCC", "gcc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
        f"-DC120_HTTP_PORT={server.server_port}", "-DC120_MOTION_POLL_MS=50", "-DC120_LIGHT_SETTLE_MS=100",
        "-O2", f'-DC120_ROOT="{root}"', str(BASE / "c120-eventd.c"), "-o", str(binary),
    ], check=True)
    for directory in ("etc", "run", "tmp", "usr/bin"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    for pin, value, direction in ((9, "1", "in"), (12, "1", "out"), (13, "0", "out"), (14, "0", "out")):
        write(root / f"sys/class/gpio/gpio{pin}/value", value + "\n")
        write(root / f"sys/class/gpio/gpio{pin}/direction", direction + "\n")
    follower = root / "sys/class/gpio/gpio13/value"
    leader = root / "sys/class/gpio/gpio12/value"
    button = root / "sys/class/gpio/gpio9/value"
    direction = root / "sys/class/gpio/gpio13/direction"
    state = root / "tmp/c120-lamps.state"
    config = root / "etc/c120-light-pins.conf"
    pidfile = root / "run/c120-eventd.pid"
    apstate = root / "run/c120-setup-ap.active"
    motion_config = root / "etc/c120-motion-light.conf"
    motion_state = root / "run/c120-motion-light.active"
    white = root / "sys/class/gpio/gpio14/value"

    def run(command, expected=0):
        command = [command] if isinstance(command, str) else command
        result = subprocess.run([str(binary), *command], capture_output=True, text=True)
        assert result.returncode == expected, result.stderr
        return result.stdout

    def motion_status():
        return json.loads(run("motion-status"))

    def arm(trigger_seconds=0):
        write(motion_config, 'C120_MOTION_LIGHT_ENABLED="1"\nC120_MOTION_LIGHT_SECONDS="1"\n'
              f'C120_MOTION_LIGHT_TRIGGER_SECONDS="{trigger_seconds}"\n')
        before = metrics["requests"]
        run("reload")
        wait_for(lambda: metrics["requests"] >= before + 4)
        time.sleep(0.15)

    def trigger():
        metrics["count"] += 1
        wait_for(lambda: white.read_text().strip() == "1" and motion_state.exists())

    run("sync")
    assert follower.read_text().strip() == "1"
    stamp = (follower.stat().st_mtime_ns, direction.stat().st_mtime_ns)
    run("sync")
    assert stamp == (follower.stat().st_mtime_ns, direction.stat().st_mtime_ns), "unchanged GPIO rewritten"
    write(state, "940\n")
    write(leader, "0\n")
    run("sync")
    assert follower.read_text().strip() == "1", "exclusive mode not respected"
    state.unlink()
    write(apstate, "")
    run("sync")
    assert follower.read_text().strip() == "1", "AP mode not respected"
    apstate.unlink()
    write(leader, "invalid\n")
    run("sync", 1)
    assert follower.read_text().strip() == "1", "failed leader read changed lights"
    write(leader, "0\n")
    run("sync")
    assert follower.read_text().strip() == "0"
    write(config, 'C120_CAMERA_LIGHT_PINS="13,13;12"\n')
    assert 'pins="13 12"' in run("status")
    write(config, 'C120_CAMERA_LIGHT_PINS="13 bad"\nC120_LIGHT_PINS_POLL="nan"\n')
    assert 'pins="12 13"' in run("status"), "invalid list was partially applied"
    write(pidfile, f"{os.getpid()}\n")
    assert "eventd=stopped" in run("status"), "unrelated process treated as daemon"
    run("stop", 1)
    write(config, 'C120_CAMERA_LIGHT_PINS="12 13"\nC120_LIGHT_PINS_POLL="0.05"\n')
    write(root / "etc/c120-eventd.conf", 'C120_RESET_POLL_DELAY="0.05"\nC120_RESET_HOLD_TICKS="3"\n')
    helper = root / "usr/bin/c120-setup-ap"
    write(helper, f'''#!/bin/sh
echo "$1" >> '{root}/transitions'
if [ "$1" = start ]; then touch '{apstate}'; else rm -f '{apstate}'; fi
''')
    helper.chmod(0o755)
    process = subprocess.Popen([str(binary), "daemon"])
    try:
        wait_for(lambda: pidfile.read_text().strip() == str(process.pid))
        assert "eventd=running" in run("status")
        run("daemon", 1)
        assert pidfile.read_text().strip() == str(process.pid), "duplicate damaged owner PID"
        write(button, "0\n")
        transitions = root / "transitions"
        wait_for(transitions.exists)
        time.sleep(0.3)
        assert transitions.read_text() == "start\n", "held button retriggered"
        write(button, "1\n")
        time.sleep(0.1)
        write(button, "0\n")
        wait_for(lambda: transitions.read_text() == "start\nstop\n")
        write(button, "1\n")
        write(config, 'C120_CAMERA_LIGHT_PINS="12"\n')
        run("reload")
        time.sleep(0.15)
        write(leader, "1\n")
        write(follower, "0\n")
        time.sleep(0.2)
        assert follower.read_text().strip() == "0", "reload did not remove follower"
        assert motion_status()["enabled"] is False and motion_status()["seconds"] == 30
        assert motion_status()["triggerSeconds"] == 3
        time.sleep(0.1)
        assert metrics["requests"] == 0, "disabled feature polled Majestic"
        arm()
        assert not motion_state.exists(), "initial historical count triggered floodlight"
        metrics["received"] += 100
        time.sleep(0.15)
        assert not motion_state.exists(), "motion outside configured regions triggered floodlight"
        trigger()
        assert motion_status()["active"] and motion_status()["remaining"] == 1
        assert leader.read_text().strip() == "1" and follower.read_text().strip() == "0", "timer changed IR"
        stamp = white.stat().st_mtime_ns
        time.sleep(0.3)
        assert white.stat().st_mtime_ns == stamp, "steady on lamp was rewritten"
        old_deadline = motion_state.read_text()
        metrics["count"] += 1
        wait_for(lambda: motion_state.read_text() != old_deadline)
        time.sleep(0.8)
        assert white.read_text().strip() == "1", "new motion did not extend timer"
        wait_for(lambda: white.read_text().strip() == "0")
        assert not motion_state.exists()
        # Scene changes caused by switching our lamp off must not re-light it.
        metrics["count"] += 1
        time.sleep(0.08)
        assert white.read_text().strip() == "0", "lamp-off exposure change retriggered timer"
        time.sleep(0.15)
        trigger()
        metrics["night"] = "0"
        wait_for(lambda: white.read_text().strip() == "0")
        metrics["count"] += 1
        time.sleep(0.15)
        assert not motion_state.exists(), "daytime motion lit floodlight"
        metrics["night"] = "1"
        time.sleep(0.15)
        assert not motion_state.exists(), "daytime count replayed on entering night"
        for invalid_night in ("-1", "nan", "2", "18446744073709551616"):
            trigger()
            metrics["night"] = invalid_night
            wait_for(lambda: white.read_text().strip() == "0")
            metrics["night"] = "1"
            time.sleep(0.15)
        trigger()
        metrics["error"] = True
        wait_for(lambda: white.read_text().strip() == "0")
        metrics["error"] = False
        time.sleep(0.15)
        trigger()
        metrics["stall"] = True
        started = time.monotonic()
        wait_for(lambda: white.read_text().strip() == "0")
        assert time.monotonic() - started < 0.8, "stalled HTTP request exceeded deadline"
        metrics["stall"] = False
        time.sleep(0.4)
        trigger()
        write(apstate, "")
        wait_for(lambda: white.read_text().strip() == "0")
        assert not motion_state.exists(), "AP mode retained automatic light ownership"
        apstate.unlink()
        time.sleep(0.15)
        trigger()
        write(motion_config, 'C120_MOTION_LIGHT_ENABLED="0"\n')
        run("reload")
        wait_for(lambda: white.read_text().strip() == "0")
        arm()
        trigger()
        with (root / "run/c120-lamps.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            motion_state.unlink()
            write(state, "white\n")
            write(white, "1\n")
        time.sleep(1.2)
        assert white.read_text().strip() == "1" and not motion_state.exists(), "manual white was overridden"
        write(motion_config, 'C120_MOTION_LIGHT_ENABLED="0"\n')
        run("reload")
        time.sleep(0.15)
        assert white.read_text().strip() == "1", "disabling timer turned off manual light"
        state.unlink()
        write(white, "0\n")
        arm(3)
        metrics["count"] += 1
        time.sleep(0.2)
        assert white.read_text().strip() == "0", "single motion event bypassed qualification"
        # A quiet poll must reset the partial qualification, not accumulate separate bursts.
        end = time.monotonic() + 1.2
        while time.monotonic() < end:
            metrics["count"] += 1
            time.sleep(0.01)
        time.sleep(0.2)
        started = time.monotonic()
        while white.read_text().strip() != "1" and time.monotonic() - started < 4:
            metrics["count"] += 1
            time.sleep(0.01)
        assert white.read_text().strip() == "1", "sustained motion did not qualify"
        assert time.monotonic() - started >= 3, "quiet gap did not reset qualification"
        wait_for(lambda: white.read_text().strip() == "0")
        arm()
        # Native counters can reset after a streamer restart; do not interpret a reset as motion.
        metrics["count"] = 0
        time.sleep(0.15)
        assert not motion_state.exists()
        trigger()
        manual = root / "run/c120-floodlight.manual"
        ir_before = (leader.read_text(), follower.read_text(), state.exists())
        assert json.loads(run(["floodlight", "on"]))["manual"] is True
        assert not motion_state.exists(), "manual floodlight did not take ownership"
        stamp = white.stat().st_mtime_ns
        assert json.loads(run(["floodlight", "on"]))["white"] is True
        assert white.stat().st_mtime_ns == stamp, "idempotent on rewrote GPIO"
        metrics["count"] += 10
        time.sleep(1.2)
        assert white.read_text().strip() == "1" and not motion_state.exists(), "timer overrode manual floodlight"
        assert json.loads(run(["floodlight", "status"])) == {"white": True, "manual": True, "available": True}
        with (root / "run/c120-lamps.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            run(["floodlight", "toggle"], 1)
        assert white.read_text().strip() == "1", "busy command changed floodlight"
        write(config, 'C120_CAMERA_LIGHT_PINS="12 14"\n')
        run(["floodlight", "off"], 1)
        assert json.loads(run(["floodlight", "status"]))["available"] is False
        write(config, 'C120_CAMERA_LIGHT_PINS=""\n')
        run(["floodlight", "invalid"], 2)
        assert json.loads(run(["floodlight", "toggle"]))["white"] is False
        assert not manual.exists(), "off did not release manual ownership"
        assert (leader.read_text(), follower.read_text(), state.exists()) == ir_before, "floodlight changed IR policy"
        metrics["count"] += 1
        time.sleep(0.08)
        assert white.read_text().strip() == "0", "manual-off exposure change retriggered timer"
        time.sleep(0.2)
        trigger()
        # AI presence is already confidence/ROI filtered; do not use raw motion counts.
        signal_file = root / 'run/c120-ai-light.signal'
        def presence(mask, age=0, interval=500):
            write(signal_file, f'1 100 {int(time.monotonic()*1000)-age} {interval} {mask}\n')
        write(motion_config, 'C120_MOTION_LIGHT_ENABLED="1"\nC120_MOTION_LIGHT_SECONDS="1"\n'
              'C120_MOTION_LIGHT_TRIGGER_SECONDS="0"\nC120_MOTION_LIGHT_SOURCE="ai"\nC120_MOTION_LIGHT_AI_MASK="1"\n')
        presence(2)
        run('reload')
        wait_for(lambda: white.read_text().strip() == '0')
        time.sleep(.2)
        metrics['count'] += 100
        assert white.read_text().strip() == '0', 'unselected pet/raw motion triggered AI-person light'
        assert motion_status()['source'] == 'ai' and motion_status()['detectorRunning']
        presence(1)
        wait_for(lambda: white.read_text().strip() == '1')
        presence(1,age=10000)
        wait_for(lambda: white.read_text().strip() == '0')
        assert not motion_status()['detectorRunning'], 'stale AI signal reported running'
        for invalid in ('2 100 0 500 1', '1 100 0 500 8', 'malformed'):
            write(signal_file,invalid+'\n'); time.sleep(.1)
            assert white.read_text().strip() == '0'
        write(motion_config, motion_config.read_text().replace('TRIGGER_SECONDS="0"','TRIGGER_SECONDS="3"'))
        run('reload'); time.sleep(.2)
        started = time.monotonic(); presence(1,interval=5000)
        wait_for(lambda: white.read_text().strip() == '1',timeout=4)
        assert time.monotonic()-started >= 3, 'AI presence bypassed sustained-detection delay'
        presence(0)
        wait_for(lambda: white.read_text().strip() == '0')
        presence(1); signal_file.unlink()
        time.sleep(.15)
        assert white.read_text().strip() == '0', 'missing AI detector signal retriggered light'
        write(config, 'C120_CAMERA_LIGHT_PINS="12"\nC120_LIGHT_PINS_RESPECT_EXCLUSIVE="1"\n')
        write(motion_config, 'C120_MOTION_LIGHT_ENABLED="1"\nC120_MOTION_LIGHT_SECONDS="1"\n'
              'C120_MOTION_LIGHT_TRIGGER_SECONDS="0"\nC120_MOTION_LIGHT_SOURCE="ai"\nC120_MOTION_LIGHT_AI_MASK="8"\n')
        run('reload'); time.sleep(.2)
        presence(1); time.sleep(.15)
        assert white.read_text().strip() == '0', 'bird-only light accepted a person'
        presence(8)
        wait_for(lambda: white.read_text().strip() == '1')
        presence(0)
        wait_for(lambda: white.read_text().strip() == '0')
        run("stop")
        process.wait(timeout=2)
        assert not pidfile.exists(), "owner PID was not removed"
        assert white.read_text().strip() == "0" and not motion_state.exists(), "shutdown left timer light on"
        write(motion_state, "999999999999\n")
        write(white, "1\n")
        process = subprocess.Popen([str(binary), "daemon"])
        wait_for(lambda: pidfile.exists() and white.read_text().strip() == "0")
        run("stop")
        process.wait(timeout=2)
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=2)
        server.shutdown()
        server.server_close()
    print("PASS native daemon: GPIO, configuration, singleton, reload, button toggling, shutdown")
    print("PASS motion floodlight: ROI-filtered events, sustained-motion delay, night gate, timer/retrigger, native failures, AP, manual ownership, restart cleanup")
    print("PASS floodlight URL helper: atomic toggle, idempotent on, manual ownership, IR preservation, busy/conflict guards")
    print("PASS AI floodlight: selected categories, raw-motion exclusion, fresh presence, stale/malformed/missing signals, sustained delay at slow analysis interval")


def test_ap(root):
    script = (BASE / "ap-recovery-plugin/files/usr/bin/c120-setup-ap").read_text()
    functions = script.split('case "${1:-status}" in')[0]
    functions = re.sub(r"/(?:var/run|var/lib|etc/init\.d|proc/sys|run|tmp)/",
                       lambda match: str(root) + match[0], functions)
    for directory in ("run", "tmp", "var/run/hostapd", "var/lib/misc", "proc/sys/vm"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    service = root / "etc/init.d/S95majestic"
    write(service, f'#!/bin/sh\necho "$1" >> "{root}/service-calls"\n')
    service.chmod(0o755)
    mocks = '''
sleep() { :; }
stop_ap_processes() { :; }
stop_setup_httpd() { :; }
ifconfig() { :; }
iwconfig() { :; }
ifdown() { :; }
ifup() { :; }
hostapd() { return 1; }
wpa_supplicant() { return 1; }
stop_service_if_running() { [ "$1" != S95majestic ] || echo "$1" >> "$SERVICE_STATE"; }
'''
    assertions = f'''
touch "$STATE"
echo S95majestic > "$SERVICE_STATE"
start_ap || exit 1
[ "$(cat "$SERVICE_STATE")" = S95majestic ] || exit 2
rm -f "$STATE" "$SERVICE_STATE"
if start_ap; then exit 3; fi
[ ! -f "$STATE" ] || exit 4
[ ! -f "$SERVICE_STATE" ] || exit 5
[ "$(cat '{root}/service-calls')" = start ] || exit 6
stop_ap
[ "$(cat '{root}/service-calls')" = start ] || exit 7
'''
    subprocess.run(["sh", "-c", functions + mocks + assertions], check=True)
    lock = root / "run/c120-setup-ap.lock"
    dispatcher = script[script.index('case "${1:-status}" in'):]
    with lock.open("w") as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        blocked = subprocess.run(["sh", "-c", functions + mocks + dispatcher, "test", "start"],
                                 capture_output=True, text=True)
        assert blocked.returncode == 1 and "already in progress" in blocked.stderr
    print("PASS AP recovery: duplicate start, failed startup restores services, redundant stop")


def test_forms(root):
    root.mkdir(parents=True)
    (root / "tmp").mkdir()
    haserl = shutil.which(os.environ.get("HASERL", "haserl"))
    assert haserl, "Haserl is required to test the web forms"
    calls = root / "wifi-calls"
    config = root / "etc/c120-light-pins.conf"
    helper = root / "light-pinsd"
    write(helper, "#!/bin/sh\necho ready\n")
    helper.chmod(0o755)
    motion_config = root / "etc/c120-motion-light.conf"
    event_helper = root / "c120-eventd"
    flood_calls = root / "floodlight-calls"
    write(event_helper, f'''#!/bin/sh
case "$1" in
status) echo eventd=running ;;
reload) : ;;
floodlight)
echo "$2" >> '{flood_calls}'
printf '{{"white":false,"manual":false,"available":true}}\\n'
;;
motion-status)
C120_MOTION_LIGHT_ENABLED=0
C120_MOTION_LIGHT_SECONDS=30
C120_MOTION_LIGHT_TRIGGER_SECONDS=3
C120_MOTION_LIGHT_SOURCE=motion
C120_MOTION_LIGHT_AI_MASK=1
[ ! -f '{motion_config}' ] || . '{motion_config}'
[ "$C120_MOTION_LIGHT_ENABLED" = 1 ] && enabled=true || enabled=false
printf '{{"enabled":%s,"seconds":%s,"triggerSeconds":%s,"active":false,"remaining":0,"running":true,"available":true,"source":"%s","aiMask":%s,"detectorRunning":true}}\\n' "$enabled" "$C120_MOTION_LIGHT_SECONDS" "$C120_MOTION_LIGHT_TRIGGER_SECONDS" "$C120_MOTION_LIGHT_SOURCE" "$C120_MOTION_LIGHT_AI_MASK"
;;
esac
''')
    event_helper.chmod(0o755)
    lamp_helper = root / "c120-lamps"
    lamp_calls = root / "lamp-calls"
    write(lamp_helper, f'#!/bin/sh\necho "$1" >> "{lamp_calls}"\nprintf \'{{"mode":"%s"}}\\n\' "$1"\n')
    lamp_helper.chmod(0o755)
    mocks = f'''
fw_setenv() {{ printf '%s\\0' "$@" >> '{calls}'; }}
nohup() {{ :; }}
cli() {{ :; }}
'''

    def render(path, fields=None, cookie="", method="GET", referer="http://camera.local/cgi-bin/c120-lights.cgi", origin="http://camera.local", query=None):
        script = re.sub(r"/(?:etc|tmp)/", lambda match: str(root) + match[0],
                        (BASE / path).read_text())
        script = script.replace("#!/usr/bin/haserl", "#!" + haserl, 1)
        script = script.replace("/usr/bin/c120-light-pinsd", str(helper))
        script = script.replace("/usr/bin/c120-eventd", str(event_helper))
        script = script.replace("/usr/bin/c120-lamps", str(lamp_helper))
        script = script.replace("<%", "<%" + mocks, 1)
        page = root / "test.cgi"
        write(page, script)
        data = urlencode(fields or {})
        env = dict(os.environ, REQUEST_METHOD=method, QUERY_STRING=query if query is not None else data if method == "GET" else "",
                   HTTP_COOKIE=cookie, HTTP_REFERER=referer, HTTP_ORIGIN=origin,
                   HTTP_HOST="camera.local", CONTENT_TYPE="application/x-www-form-urlencoded",
                   CONTENT_LENGTH=str(len(data)) if method == "POST" else "0")
        result = subprocess.run([haserl, str(page)], env=env,
                                input=data.encode() if method == "POST" else b"",
                                capture_output=True, check=True)
        assert not result.stderr, result.stderr
        return result.stdout.decode()

    light = "runtime-overlay/var/www/cgi-bin/c120-light.cgi"
    for mode in ('off', '850', '940', 'both', 'ir', '850940', 'white', 'status'):
        output = render(light, {'mode': mode})
        assert output.startswith('HTTP/1.1 200 OK\nContent-type: application/json\n')
        assert lamp_calls.read_text().splitlines()[-1] == mode
    render(light, query='mode=%38%35%30')
    assert lamp_calls.read_text().splitlines()[-1] == '850', 'mode was not URL-decoded'
    for options in ({'cookie': 'mode=white'}, {'fields': {'mode': 'white'}, 'method': 'POST'},
                    {'fields': {'mode': 'white;touch injected'}}, {'fields': {'mode': '*'}}):
        render(light, **options)
        assert lamp_calls.read_text().splitlines()[-1] == 'status'
    assert not (root / 'injected').exists()

    wifi = "ap-recovery-plugin/files/var/www/cgi-bin/c120-wifi-setup.cgi"
    assert '<form method="get"' in render(wifi, cookie="apply=1; ssid=ignored")
    assert not calls.exists(), "cookies changed Wi-Fi settings"
    ssid, psk = 'Lab + & " <tag>', r'Pass+%&\\with spaces'
    output = render(wifi, {"apply": "1", "ssid": ssid, "psk": psk})
    assert calls.read_bytes() == b"wlanssid\0" + ssid.encode() + b"\0wlanpass\0" + psk.encode() + b"\0"
    assert "Saved Wi-Fi settings" in output and "<tag>" not in output

    lights = "runtime-overlay/var/www/cgi-bin/c120-light-pins.cgi"
    assert 'value="12 13"' in render(lights)
    write(config, 'C120_CAMERA_LIGHT_PINS="13 12"\nC120_LIGHT_PINS_POLL="0.75"\nC120_LIGHT_PINS_RESPECT_EXCLUSIVE="0"\n')
    output = render(lights)
    assert 'value="13 12"' in output and 'value="0.75"' in output and " checked" not in output
    output = render(lights, {"apply": "1", "pins": "12,13;14", "poll": "0.25", "exclusive": "1"})
    assert "Saved" in output and 'C120_CAMERA_LIGHT_PINS="12 13 14"' in config.read_text()
    saved = config.read_bytes()
    for fields, message in (({"pins": "12,256", "poll": "0.25"}, "Invalid GPIO list"),
                            ({"pins": "12,13", "poll": "invalid"}, "Invalid poll value")):
        assert message in render(lights, {"apply": "1", **fields})
        assert config.read_bytes() == saved, "invalid input changed configuration"
    render(lights, {"apply": "1", "pins": "12 13"})
    assert 'C120_LIGHT_PINS_RESPECT_EXCLUSIVE="0"' in config.read_text()
    page = (BASE / "runtime-overlay/var/www/cgi-bin/c120-lights.cgi").read_text()
    page = re.sub(r"<%in p/(?:common|header|footer).cgi %>", "", page)
    write(root / "lights.cgi", page)
    output = subprocess.run([haserl, str(root / "lights.cgi")],
                            env=dict(os.environ, REQUEST_METHOD="GET", QUERY_STRING=""),
                            capture_output=True, text=True, check=True).stdout
    for mode in ("off", "850", "940", "both", "white"):
        assert f'id="c120-{mode}"' in output
    assert "850 + 940 nm" in output and 'role="alert"' in output
    assert 'class="mj-seg-in"' in output and 'class="mj-seg-lbl"' in output
    assert "get_metrics" not in page and "get_night" not in page
    assert not (BASE / "runtime-overlay/var/www/cgi-bin/preview.cgi").exists()
    for control in ("c120-motion-enabled", "c120-motion-seconds", "c120-motion-trigger-seconds", "c120-motion-status"):
        assert f'id="{control}"' in output
    motion = "runtime-overlay/var/www/cgi-bin/c120-motion-light.cgi"
    output = render(motion, {"enabled": "1", "seconds": "60"})
    assert output.startswith("HTTP/1.1 200 OK\nContent-Type: application/json\n")
    assert '"enabled":false' in output
    assert not motion_config.exists(), "GET request changed timer settings"
    output = render(motion, {"enabled": "1", "seconds": "30"}, method="POST")
    assert '"enabled":true' in output and '"seconds":30' in output and '"triggerSeconds":3' in output
    saved = motion_config.read_bytes()
    for duration in ("0", "601", "1.5", "nan", "1;touch injected", "99999999999999999999"):
        assert "400 Bad Request" in render(motion, {"enabled": "1", "seconds": duration}, method="POST")
        assert motion_config.read_bytes() == saved
    for delay in ("", "-1", "601", "1.5", "nan", "1;touch injected", "99999999999999999999"):
        assert "400 Bad Request" in render(motion, {"enabled": "1", "seconds": "30", "triggerSeconds": delay}, method="POST")
        assert motion_config.read_bytes() == saved
    for delay in ("0", "3", "600"):
        output = render(motion, {"enabled": "1", "seconds": "30", "triggerSeconds": delay}, method="POST")
        assert f'"triggerSeconds":{delay}' in output
    saved = motion_config.read_bytes()
    fields = {"enabled": "0", "seconds": "10"}
    assert "403 Forbidden" in render(motion, fields, method="POST", referer="")
    assert "403 Forbidden" in render(motion, fields, method="POST", referer="http://camera.local.other/page")
    assert "403 Forbidden" in render(motion, fields, method="POST", origin="http://other.local")
    assert motion_config.read_bytes() == saved, "cross-origin or untrusted request changed settings"
    assert '"enabled":false' in render(motion, fields, method="POST")
    output = render(motion, {'enabled':'1','seconds':'30','source':'ai','aiMask':'3'},method='POST')
    assert '"source":"ai"' in output and '"aiMask":3' in output
    output = render(motion, {'enabled':'1','seconds':'10'},method='POST')
    assert '"source":"ai"' in output and '"aiMask":3' in output, 'legacy caller reset AI selection'
    saved = motion_config.read_bytes()
    for mask in ('8','15'):
        output = render(motion, {'enabled':'1','seconds':'30','source':'ai','aiMask':mask},method='POST')
        assert f'"aiMask":{mask}' in output
    saved = motion_config.read_bytes()
    for change in [{'source':'ai','aiMask':'0'},{'source':'unknown'},{'aiMask':'16'},{'aiMask':'1;bad'}]:
        assert '400 Bad Request' in render(motion,{'enabled':'1','seconds':'10',**change},method='POST')
        assert motion_config.read_bytes() == saved
    flood = "runtime-overlay/var/www/cgi-bin/c120-floodlight.cgi"
    output = render(flood, cookie="action=on")
    assert output.startswith("HTTP/1.1 200 OK\nContent-Type: application/json\nCache-Control: no-store\n")
    assert flood_calls.read_text() == "toggle\n", "cookie overrode default toggle"
    for action in ("on", "off", "status", "toggle"):
        render(flood, {"action": action}, referer="", origin="")
        assert flood_calls.read_text().splitlines()[-1] == action
    render(flood, {"action": "off"}, method="POST")
    assert flood_calls.read_text().splitlines()[-1] == "off"
    saved = flood_calls.read_bytes()
    assert "405 Method Not Allowed" in render(flood, method="HEAD")
    assert "400 Bad Request" in render(flood, {"action": "on;touch injected"})
    assert "403 Forbidden" in render(flood, referer="http://other.local/page")
    assert "403 Forbidden" in render(flood, origin="http://other.local")
    assert flood_calls.read_bytes() == saved, "invalid request reached GPIO helper"
    print("PASS forms: real Haserl decoding, Wi-Fi saves, GPIO settings and validation")
    print("PASS light CGI: URL decoding, mode whitelist, GET-only input and cookie isolation")
    print("PASS motion form: POST-only writes, same-origin checks, duration bounds, enable/disable")
    print("PASS floodlight CGI: toggle/on/off/status, GET/POST, cookies, method and origin guards")


def test_dashboard(root):
    root.mkdir(parents=True)
    fix = BASE / "dashboard-luminance.sed"
    for source in (
        'makeChart("#ch-luma",{h:110,lo:0,hi:255,colors:[C1],bands:[{from:0,to:20,color:"rgba(255,193,7,.10)"}]});now.textContent=v.isp_avelum+" / 255";',
        "makeChart('#ch-luma', {h: 110, lo: 0, hi: 255, colors: [C1],\n"
        "bands: [{ from: 0, to: 20, color: 'rgba(255,193,7,.10)' }],\n"
        "});now.textContent = v.isp_avelum + ' / 255';",
    ):
        source += '\nconst unrelated={lo:5,hi:255}; const msg="255 pixels";'
        def patch(text):
            return subprocess.run(["sed", "-f", str(fix)], input=text, text=True,
                                  capture_output=True, check=True).stdout
        result = patch(source)
        assert re.search(r"hi:\s*null", result)
        assert ' / 255' not in result and ' raw' in result
        assert 'bands:[]' in result and 'to:20' not in result
        assert 'const unrelated={lo:5,hi:255}' in result and '255 pixels' in result
        assert patch(result) == result, "reinstall changed the patched dashboard"
    print("PASS dashboard: raw SDK luminance, automatic range, no 8-bit band, idempotent fixup")


def test_memory(root):
    root.mkdir(parents=True)
    haserl = shutil.which(os.environ.get("HASERL", "haserl"))
    assert haserl, "Haserl is required to test the memory endpoint"
    cmdline = root / "cmdline"
    heap = root / "mma_heap_name0"
    source = (BASE / "runtime-overlay/var/www/cgi-bin/c120-memory.cgi").read_text()
    source = source.replace("/proc/cmdline", str(cmdline)).replace(
        "/proc/mi_modules/mi_sys_mma/mma_heap_name0", str(heap))
    page = root / "memory.cgi"
    write(page, source)
    cmdline.write_text("console=ttyS0 LX_MEM=0x4000000 mma_heap=mma_heap_name0,miu=0,sz=0x2000000 cma=2M\n")
    heap.write_text("heap pa_start length avail\n      mma_heap_name0 22000000 2000000 600000\n")

    def read(method="GET"):
        result = subprocess.run([haserl, str(page)],
                                env=dict(os.environ, REQUEST_METHOD=method, QUERY_STRING=""),
                                capture_output=True, text=True, check=True)
        assert not result.stderr, result.stderr
        return result.stdout

    response = read()
    assert "200 OK" in response
    assert json.loads(response.split("\n\n", 1)[1]) == {
        "physicalBytes": 64 * 1048576,
        "mediaBytes": 32 * 1048576,
        "mediaFreeBytes": 6 * 1048576,
    }
    heap.unlink()
    assert "mediaFreeBytes" not in read()
    cmdline.write_text("LX_MEM=0x4000000 mma_heap=mma_heap_name0,miu=0,sz=oops\n")
    assert "503 Service Unavailable" in read()
    assert "405 Method Not Allowed" in read("POST")

    page = root / "dashboard.cgi"
    write(page, '<div class="mj-cap">Memory</div>\n'
          '<span class="mj-cap">Memory &mdash; what is holding it</span>\n'
          '<div class="x-small text-secondary" id="st-mem-note"></div>\n')
    snippet = BASE / "dashboard-memory.html"
    renamed = subprocess.run(['sed', '-e',
        's|<div class="mj-cap">Memory</div>|<div class="mj-cap">Linux memory</div>|',
        '-e', 's|<span class="mj-cap">Memory &mdash; what is holding it</span>|'
        '<span class="mj-cap">Linux memory over time</span>|', str(page)],
        capture_output=True, text=True, check=True).stdout
    first = subprocess.run(['sed', f'/id="st-mem-note"><\\/div>/r {snippet}'],
                           input=renamed, capture_output=True, text=True, check=True).stdout
    assert first.count('id="c120-memory-map"') == 1
    assert "Linux memory over time" in first and "Linux memory</div>" in first
    assert first.index('id="st-mem-note"') < first.index('id="c120-memory-map"')
    assert "id=\"c120-memory-map\"" in snippet.read_text()
    print("PASS memory: boot map, live MMA free, unavailable fallback, GET-only, dashboard insertion")


def test_endpoint_list(root):
    root.mkdir(parents=True)
    page = root / "stream-urls.cgi"
    page.write_text('<dl>\n<dd>Toggle camera light.</dd>\n</dl>\n')
    snippet = BASE / "floodlight-url.html"
    result = subprocess.run(["sed", f'/<dd>Toggle camera light\\.<\\/dd>/r {snippet}', str(page)],
                            check=True, capture_output=True, text=True).stdout
    assert result.count('/cgi-bin/c120-floodlight.cgi') == 1
    assert 'class="ep-host"' in result and 'class="cp2cb"' in result
    assert 'Toggle white floodlight.' in result
    assert "! grep -q 'c120-floodlight.cgi'" in (BASE / "install-runtime.sh").read_text()
    print("PASS endpoint list: existing URL styling and guarded installer insertion")


def test_build(root):
    repo = BASE.parents[1]
    for sensor in ("sc430ai", ""):
        build = root / (sensor or "generic")
        build.mkdir(parents=True)
        package = str(repo / "general/package/sigmastar-osdrv-sensors/sigmastar-osdrv-sensors.mk").replace(" ", "\\ ")
        makefile = f'''
BR2_EXTERNAL := {repo}/general
OPENIPC_SOC_VENDOR := sigmastar
OPENIPC_SOC_FAMILY := infinity6c
OPENIPC_SNS_MODEL := {sensor}
include {package}
{build}/stamp:
\t$(SIGMASTAR_OSDRV_SENSORS_COPY_LOCAL_SOURCES)
'''
        subprocess.run(["make", "-s", "-f", "-", str(build / "stamp")], input=makefile,
                       text=True, check=True)
        copied = build / "sigmastar/infinity6c/sensor_sc430ai_mipi.c"
        assert copied.exists() == bool(sensor)
        if sensor:
            assert copied.read_bytes() == (repo / "sigmastar/infinity6c/sensor_sc430ai_mipi.c").read_bytes()
    fixture = root / "firmware"
    boards = ["ssc377_lite_tp-link-tapo-c120-v1", "ssc377_lite_tp-link-tapo-c120-v1-sc438hai", "ssc377_lite"]
    paths = ["Makefile", "general/openipc.fragment"] + [
        f"br-ext-chip-sigmastar/configs/{board}_defconfig" for board in boards]
    for path in paths:
        target = fixture / path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / path, target)
    (fixture / "output").mkdir()
    for board in boards:
        subprocess.run(["make", "-s", "-o", "prepare", f"BOARD={board}",
                        f"PWD={fixture}", "BR_MAKE=true", "defconfig"], cwd=fixture,
                       check=True, capture_output=True)
        config = (fixture / "output/openipc_defconfig").read_text()
        overlays = [line for line in config.splitlines() if line.startswith("BR2_ROOTFS_OVERLAY=")]
        assert "$(BR2_EXTERNAL)/overlay" in overlays[-1]
        assert ("board/tapo-c120/overlay" in overlays[-1]) == (board != boards[-1])
        assert ("board/tapo-c120-sc438hai/overlay" in overlays[-1]) == (board == boards[1])
    assert not (repo / "general/overlay/usr/share/openipc/customizer.sh").exists()
    config = root / ".config"
    write(config, 'BR2_OPENIPC_SNS_MODEL="sc430ai"\nBR2_PACKAGE_SIGMASTAR_OSDRV_SENSORS=y\n')
    target = root / "target"
    (target / "lib/modules").mkdir(parents=True)
    check = ["sh", str(repo / ".github/scripts/check_target_modules.sh"), str(config), str(target)]
    result = subprocess.run(check, capture_output=True, text=True)
    assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
    write(target / "lib/modules/sensor_sc430ai_mipi.ko", "fixture\n")
    result = subprocess.run(check, capture_output=True, text=True)
    assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
    write(target / "etc/sensors/sc430ai.bin", "fixture\n")
    subprocess.run(check, capture_output=True, check=True)
    write(config, 'BR2_OPENIPC_SNS_MODEL="sc438hai"\nBR2_PACKAGE_SIGMASTAR_OSDRV_SENSORS=y\n')
    assert subprocess.run(check, capture_output=True).returncode == 1
    write(target / "lib/modules/5.10.61/sigmastar/sensor_sc438hai_mipi.ko", "fixture\n")
    assert subprocess.run(check, capture_output=True).returncode == 1
    write(target / "etc/sensors/sc438hai.bin", "fixture\n")
    subprocess.run(check, capture_output=True, check=True)
    print("PASS builds: exact board selection, isolated C120 defaults, local sensor source destination")


if __name__ == "__main__":
    test_payloads()
    with tempfile.TemporaryDirectory(prefix="c120-test-") as tmp:
        root = Path(tmp)
        test_daemon(root / "native")
        test_ap(root / "ap")
        test_forms(root / "forms")
        test_dashboard(root / "dashboard")
        test_memory(root / "memory")
        test_endpoint_list(root / "endpoints")
        test_build(root / "build")

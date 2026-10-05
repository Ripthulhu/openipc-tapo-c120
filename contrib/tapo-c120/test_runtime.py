"""Run on Linux with gcc and Haserl. GPIOs/services are simulated under a temp directory."""
import os
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import re
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlencode


BASE = Path(__file__).resolve().parent


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
        result = subprocess.run([str(binary), command], capture_output=True, text=True)
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
    write(event_helper, f'''#!/bin/sh
case "$1" in
status) echo eventd=running ;;
reload) : ;;
motion-status)
C120_MOTION_LIGHT_ENABLED=0
C120_MOTION_LIGHT_SECONDS=30
C120_MOTION_LIGHT_TRIGGER_SECONDS=3
[ ! -f '{motion_config}' ] || . '{motion_config}'
[ "$C120_MOTION_LIGHT_ENABLED" = 1 ] && enabled=true || enabled=false
printf '{{"enabled":%s,"seconds":%s,"triggerSeconds":%s,"active":false,"remaining":0,"running":true,"available":true}}\\n' "$enabled" "$C120_MOTION_LIGHT_SECONDS" "$C120_MOTION_LIGHT_TRIGGER_SECONDS"
;;
esac
''')
    event_helper.chmod(0o755)
    mocks = f'''
fw_setenv() {{ printf '%s\\0' "$@" >> '{calls}'; }}
nohup() {{ :; }}
cli() {{ :; }}
'''

    def render(path, fields=None, cookie="", method="GET", referer="http://camera.local/cgi-bin/c120-lights.cgi", origin="http://camera.local"):
        script = re.sub(r"/(?:etc|tmp)/", lambda match: str(root) + match[0],
                        (BASE / path).read_text())
        script = script.replace("#!/usr/bin/haserl", "#!" + haserl, 1)
        script = script.replace("/usr/bin/c120-light-pinsd", str(helper))
        script = script.replace("/usr/bin/c120-eventd", str(event_helper))
        script = script.replace("<%", "<%" + mocks, 1)
        page = root / "test.cgi"
        write(page, script)
        data = urlencode(fields or {})
        env = dict(os.environ, REQUEST_METHOD=method, QUERY_STRING=data if method == "GET" else "",
                   HTTP_COOKIE=cookie, HTTP_REFERER=referer, HTTP_ORIGIN=origin,
                   HTTP_HOST="camera.local", CONTENT_TYPE="application/x-www-form-urlencoded",
                   CONTENT_LENGTH=str(len(data)) if method == "POST" else "0")
        result = subprocess.run([haserl, str(page)], env=env,
                                input=data.encode() if method == "POST" else b"",
                                capture_output=True, check=True)
        assert not result.stderr, result.stderr
        return result.stdout.decode()

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
    print("PASS forms: real Haserl decoding, Wi-Fi saves, GPIO settings and validation")
    print("PASS motion form: POST-only writes, same-origin checks, duration bounds, enable/disable")


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
    boards = ["ssc377_lite_tp-link-tapo-c120-v1", "ssc377_lite"]
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
        assert ("board/tapo-c120/overlay" in overlays[-1]) == (board == boards[0])
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
    print("PASS builds: exact board selection, isolated C120 defaults, local sensor source destination")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="c120-test-") as tmp:
        root = Path(tmp)
        test_daemon(root / "native")
        test_ap(root / "ap")
        test_forms(root / "forms")
        test_dashboard(root / "dashboard")
        test_build(root / "build")

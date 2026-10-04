"""Run on Linux with gcc and Haserl. GPIOs/services are simulated under a temp directory."""
import os
import fcntl
import re
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile
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
    binary = root / "c120-eventd"
    subprocess.run([
        os.environ.get("HOSTCC", "gcc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-O2", f'-DC120_ROOT="{root}"', str(BASE / "c120-eventd.c"), "-o", str(binary),
    ], check=True)
    for directory in ("etc", "run", "tmp", "usr/bin"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    for pin, value, direction in ((9, "1", "in"), (12, "1", "out"), (13, "0", "out")):
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

    def run(command, expected=0):
        result = subprocess.run([str(binary), command], capture_output=True, text=True)
        assert result.returncode == expected, result.stderr
        return result.stdout

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
        run("stop")
        process.wait(timeout=2)
        assert not pidfile.exists(), "owner PID was not removed"
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=2)
    print("PASS native daemon: GPIO, configuration, singleton, reload, button toggling, shutdown")


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
    mocks = f'''
fw_setenv() {{ printf '%s\\0' "$@" >> '{calls}'; }}
nohup() {{ :; }}
cli() {{ :; }}
'''

    def render(path, fields=None, cookie=""):
        script = re.sub(r"/(?:etc|tmp)/", lambda match: str(root) + match[0],
                        (BASE / path).read_text())
        script = script.replace("#!/usr/bin/haserl", "#!" + haserl, 1)
        script = script.replace("/usr/bin/c120-light-pinsd", str(helper))
        script = script.replace("<%", "<%" + mocks, 1)
        page = root / "test.cgi"
        write(page, script)
        env = dict(os.environ, REQUEST_METHOD="GET", QUERY_STRING=urlencode(fields or {}),
                   HTTP_COOKIE=cookie)
        result = subprocess.run([haserl, str(page)], env=env,
                                capture_output=True, text=True, check=True)
        assert not result.stderr, result.stderr
        return result.stdout

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
    assert "get_metrics" not in page and "get_night" not in page
    assert not (BASE / "runtime-overlay/var/www/cgi-bin/preview.cgi").exists()
    print("PASS forms: real Haserl decoding, Wi-Fi saves, GPIO settings and validation")


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
        test_build(root / "build")

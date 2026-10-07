"""Linux checks for native C120 QHD defaults and the firmware lifecycle."""
import os
from pathlib import Path
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[2]


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def test_defaults(root):
    customizer = REPO / "br-ext-chip-sigmastar/board/tapo-c120/overlay/usr/share/openipc/customizer.sh"
    result = subprocess.run(["sh", "-c", 'cli() { echo "$*"; }; fw_setenv() { :; };\n' +
                             customizer.read_text().split("# The stock crontab")[0]],
                            check=True, capture_output=True, text=True)
    for setting in (".video0.size 2560x1440", ".video0.fps 30", ".video0.bitrate 10000",
                    ".video0.gopSize 2", ".isp.exposure 33", ".motionDetect.visualize false",
                    ".nightMode.lightMonitor true", ".nightMode.colorToGray true",
                    ".nightMode.autoNightGain 16",
                    ".nightMode.autoDayGain 2", ".nightMode.autoNightDelay 15",
                    ".nightMode.autoDayDelay 60", ".nightMode.irCut auto",
                    ".nightMode.backlight auto", ".nightMode.backlightInvert false",
                    ".nightMode.irCutPin1 81", ".nightMode.irCutSingleInvert true",
                    ".nightMode.backlightPin 12", ".audio.enabled true",
                    ".audio.codec opus", ".audio.srate 48000", ".audio.volume 50",
                    ".audio.speakerPin 43", ".audio.speakerPinInvert false",
                    ".audio.speakerPinHoldMs 2000", ".audio.outputEnabled true",
                    ".audio.outputVolume 80"):
        assert "-s " + setting in result.stdout
    for key in ("lightSensorPin", "minThreshold", "maxThreshold", "irCutPin2"):
        assert ".nightMode." + key not in result.stdout, "native automatic switching must not use legacy inputs"
    config = REPO / "br-ext-chip-sigmastar/configs/ssc377_lite_tp-link-tapo-c120-v1_defconfig"
    assert "C120_QHD" not in config.read_text()
    assert "BR2_TARGET_ROOTFS_SQUASHFS_EXTREME_COMP=y" in config.read_text()
    assert not (REPO / "general/package/c120-qhd/Config.in").exists()
    print("PASS C120 defaults: native Majestic, 1440p/30fps, automatic day/night, no preload hook")


def test_pruning(root):
    script = REPO / "general/scripts/rootfs_script.sh"
    for scenario in ("unused", "used", "glibc", "kmod"):
        target = root / scenario
        for directory in ("lib", "usr/lib", "bin", "sbin", "usr/bin", "usr/sbin"):
            (target / directory).mkdir(parents=True)
        # Pruning uses the normal startup validator; these binaries are not executed.
        for name in ("init", "bin/sh", "sbin/init", "linuxrc", "usr/sbin/cli"):
            write(target / name, "\x7fELF fixture\n")
            (target / name).chmod(0o755)
        config = target / ".config"
        write(config, "BR2_TOOLCHAIN_USES_" + ("GLIBC" if scenario == "glibc" else "MUSL") + "=y\n")
        for lib in ("libgcc_s", "libatomic"):
            write(target / f"lib/{lib}.so.1", "unused library\n")
        if scenario == "used":
            write(target / "usr/bin/reference", "ELF NEEDED libgcc_s.so.1\0dlopen libatomic.so.1\n")
        if scenario == "kmod":
            write(target / "bin/kmod", "kmod\n")
        module = target / "lib/modules/5.10"
        for name in ("modules.dep", "modules.alias", "modules.dep.bin", "modules.builtin.modinfo"):
            write(module / name, "index\n")
        env = dict(os.environ, TARGET_DIR=str(target), BR2_CONFIG=str(config), BINARIES_DIR=str(target / "images"),
                   BR2_EXTERNAL_GENERAL_PATH=str(REPO / "general"), OPENIPC_SOC_MODEL="fixture",
                   OPENIPC_VARIANT="lite")
        subprocess.run(["bash", str(script)], env=env, check=True)
        for lib in ("libgcc_s", "libatomic"):
            assert (target / f"lib/{lib}.so.1").exists() == (scenario in ("used", "glibc"))
        assert (module / "modules.dep.bin").exists() == (scenario == "kmod")
        assert (module / "modules.builtin.modinfo").exists() == (scenario == "kmod")
        assert (module / "modules.dep").exists() and (module / "modules.alias").exists()
    print("PASS pruning: unused/used musl libraries, glibc retention, BusyBox/kmod indexes")


def test_missing_config(root):
    root.mkdir(parents=True)
    config = root / "majestic.yaml"
    script = (REPO / "general/overlay/usr/sbin/extutils").read_text()
    write(root / "cli", script.replace("MAJESTIC_CFG=/etc/majestic.yaml", f"MAJESTIC_CFG={config}"))
    # Model yaml-cli's missing-input refusal, without signalling a host process.
    write(root / "yaml-cli", """#!/bin/sh
while [ $# -gt 0 ]; do
    case "$1" in -i|--input) shift; input=$1 ;; esac
    shift
done
test -f "$input"
""")
    write(root / "pidof", "#!/bin/sh\nexit 1\n")
    for name in ("cli", "yaml-cli", "pidof"):
        (root / name).chmod(0o755)
    env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"])
    for args in (("-s", ".video0.fps", "30"), ("-g", ".video0.fps")):
        subprocess.run([str(root / "cli"), *args], env=env, check=True)
        assert config.exists(), "first CLI call needs an empty input file"
        config.unlink()
    other = root / "sensor.yaml"
    other.write_text("sensor: unchanged\n")
    subprocess.run([str(root / "cli"), "-i", str(other), "-g", ".sensor"], env=env, check=True)
    assert not config.exists(), "a sensor CLI call must not create Majestic's config"
    config.write_text("video0:\n  fps: 30\n")
    subprocess.run([str(root / "cli"), "-g", ".video0.fps"], env=env, check=True)
    assert config.read_text() == "video0:\n  fps: 30\n", "existing settings must survive"
    print("PASS sparse config: first write/read, explicit input, existing settings preserved")


def test_speaker_gpio(root):
    root.mkdir(parents=True)
    gpio = root / "gpio43"
    script = (REPO / "br-ext-chip-sigmastar/board/tapo-c120/overlay/usr/share/openipc/muxes.sh").read_text()
    script = script.replace("/sys/class/gpio", str(root))
    assert "sh /usr/share/openipc/muxes.sh" in (REPO / "general/overlay/etc/init.d/S30customizer").read_text()
    mocks = '''echo() {
        if [ "$1" = 43 ]; then
            mkdir -p "$PIN"
            printf 'in\\n' > "$PIN/direction"
        fi
        printf '%s\\n' "$1"
    }
'''
    subprocess.run(["sh", "-s"], input=mocks + script, text=True, check=True)
    assert (root / "export").read_text() == "43\n"
    assert (gpio / "direction").read_text() == "low\n", "new amplifier must start off"
    write(gpio / "direction", "out\n")
    write(gpio / "value", "1\n")
    write(root / "export", "already exported\n")
    subprocess.run(["sh", "-s"], input=mocks + script, text=True, check=True)
    assert (gpio / "direction").read_text() == "out\n"
    assert (gpio / "value").read_text() == "1\n", "do not interrupt active playback on a repeated run"
    assert (root / "export").read_text() == "already exported\n"
    print("PASS C120 speaker GPIO: boot hook, low initialization, repeat preserves playback")


def test_init(root):
    root.mkdir(parents=True)
    envfile = root / "majestic.env"
    write(envfile, "export QHD_TEST=ready\n")
    script = (REPO / "general/package/majestic/files/S95majestic").read_text()
    script = script.replace("/etc/default/majestic", str(envfile))
    script = script.replace("/etc/TZ", str(root / "TZ"))
    write(root / "TZ", "UTC0\n")
    script = script.replace('"/var/run/$DAEMON.pid"', '"' + str(root / "majestic.pid") + '"')
    state = root / "running"
    calls = root / "calls"
    mocks = f'''
sleep() {{ :; }}
start-stop-daemon() {{
    case " $* " in *" -p "*) exit 9 ;; esac
    case " $* " in *" -x /usr/bin/majestic "*) ;; *) exit 10 ;; esac
    case " $* " in
        *" -t "*) [ -f '{state}' ] ;;
        *" -s 1 "*) echo reload >> '{calls}' ;;
        *" -S "*)
            [ "$QHD_TEST" = ready ] && [ "$TZ" = UTC0 ] || exit 11
            [ ! -f '{state}' ] || return 1
            echo start >> '{calls}'; touch '{state}' ;;
        *) [ "${{STUCK:-0}}" = 1 ] || rm -f '{state}' ;;
    esac
}}
'''

    def run(action, expected=0, **env):
        result = subprocess.run(["bash", "-s", "--", action], input=mocks + script,
                                text=True, capture_output=True, env=dict(os.environ, **env))
        assert result.returncode == expected, (result.stdout, result.stderr)

    run("start")
    run("start")
    assert calls.read_text() == "start\n", "duplicate daemon started"
    run("reload")
    run("restart", expected=1, STUCK="1")
    assert calls.read_text() == "start\nreload\n", "restarted after failed stop"
    run("restart")
    assert calls.read_text() == "start\nreload\nstart\n"
    run("stop")
    run("stop")
    assert not state.exists()
    print("PASS Majestic init: environment/TZ, duplicate start, reload, failed-stop guard")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="c120-qhd-test-") as tmp:
        root = Path(tmp)
        test_defaults(root / "defaults")
        test_missing_config(root / "missing-config")
        test_speaker_gpio(root / "speaker-gpio")
        test_pruning(root / "pruning")
        test_init(root / "init")

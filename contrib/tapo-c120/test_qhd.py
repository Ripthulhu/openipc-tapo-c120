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
    for setting in (".video0.size 2560x1440", ".video0.fps 20", ".video0.bitrate 10000",
                    ".video0.gopSize 2", ".isp.exposure 33", ".motionDetect.visualize false"):
        assert "-s " + setting in result.stdout
    config = REPO / "br-ext-chip-sigmastar/configs/ssc377_lite_tp-link-tapo-c120-v1_defconfig"
    assert "C120_QHD" not in config.read_text()
    assert "BR2_TARGET_ROOTFS_SQUASHFS_EXTREME_COMP=y" in config.read_text()
    assert not (REPO / "general/package/c120-qhd/Config.in").exists()
    print("PASS QHD defaults: native Majestic, 1440p/20fps, no preload hook")


def test_pruning(root):
    script = REPO / "general/scripts/rootfs_script.sh"
    for scenario in ("unused", "used", "glibc", "kmod"):
        target = root / scenario
        for directory in ("lib", "usr/lib", "bin", "sbin", "usr/bin", "usr/sbin"):
            (target / directory).mkdir(parents=True)
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
        test_pruning(root / "pruning")
        test_init(root / "init")

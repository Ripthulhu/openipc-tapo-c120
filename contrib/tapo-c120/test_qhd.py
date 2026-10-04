"""Linux checks for the C120 QHD shim, package, defaults and upstream backports."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[2]
PACKAGE = REPO / "general/package/c120-qhd"


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def test_ioctl(root):
    source = root / "test.c"
    write(source, f'''
#define readlink fake_readlink
#define ioctl c120_ioctl
#include "{PACKAGE}/src/c120-qhd.c"
#undef ioctl
#undef readlink
#include <assert.h>
static unsigned requests[4], count, last_queue;
static int fail_depth;
ssize_t fake_readlink(const char *path, char *buf, size_t size) {{
    const char *device = !strcmp(path, "/proc/self/fd/7") ? "/dev/mi_sys" : "/dev/null";
    assert(strlen(device) <= size);
    memcpy(buf, device, strlen(device));
    return strlen(device);
}}
static int mock_ioctl(int fd, request_t request, ...) {{
    (void)fd;
    requests[count++] = request;
    va_list ap;
    va_start(ap, request);
    envelope_t *e = va_arg(ap, envelope_t *);
    va_end(ap);
    if (request == SET_DEPTH) {{
        depth_t *d = (void *)(uintptr_t)e->data;
        assert(e->size == sizeof(*d));
        last_queue = d->queue;
        if (fail_depth) {{ errno = ENOMEM; return -1; }}
    }}
    return 0;
}}
int main(void) {{
    next_ioctl = mock_ioctl;
    bind_t b = {{.src = {{34,0,0,0}}, .dst = {{2,0,0,0}}, .type = 1}};
    envelope_t e = {{0,0,sizeof(b),(uintptr_t)&b}};
    assert(c120_ioctl(7, BIND_PORTS, &e) == 0);
    assert(count == 2 && requests[0] == SET_DEPTH && requests[1] == BIND_PORTS && last_queue == 2);
    count = 0; fail_depth = 1;
    assert(c120_ioctl(7, BIND_PORTS, &e) == -1 && errno == ENOMEM && count == 1);
    fail_depth = 0;
    for (int mode = 0; mode < 6; mode++) {{
        b.src.channel = mode == 0; b.dst.port = mode == 1;
        b.type = mode == 2 ? 0 : 1; e.soc = mode == 3;
        e.size = mode == 4 ? 1 : sizeof(b);
        count = 0;
        assert(c120_ioctl(mode == 5 ? 8 : 7, BIND_PORTS, &e) == 0 && count == 1);
    }}
    depth_t d = {{{{34,0,0,0}},0,3}};
    e = (envelope_t){{0,0,sizeof(d),(uintptr_t)&d}};
    count = 0;
    assert(c120_ioctl(7, SET_DEPTH, &e) == 0 && d.queue == 2);
    d.port.port = 1; d.queue = 3;
    count = 0;
    assert(c120_ioctl(7, SET_DEPTH, &e) == 0 && d.queue == 3 && count == 1);
    d.port.port = 0; d.user = 1;
    count = 0;
    assert(c120_ioctl(7, SET_DEPTH, &e) == 0 && d.queue == 3 && count == 1);
    count = 0;
    assert(c120_ioctl(7, 1234, NULL) == 0 && count == 1);
}}
''')
    binary = root / "test"
    subprocess.run([os.environ.get("HOSTCC", "gcc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(source), "-ldl", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print("PASS QHD ioctl: ABI, queue-before-bind, failure, device/channel/envelope guards")


def test_package(root):
    build = root / "build"
    build.mkdir(parents=True)
    shutil.copy2(PACKAGE / "src/c120-qhd.c", build)
    target = root / "target"
    package = str(PACKAGE / "c120-qhd.mk").replace(" ", "\\ ")
    makefile = f'''
C120_QHD_PKGDIR := {PACKAGE}
TARGET_CC := {os.environ.get('HOSTCC', 'gcc')}
TARGET_CFLAGS := -Os
INSTALL := install
TARGET_DIR := {target}
include {package}
{build}/stamp:
\t$(C120_QHD_BUILD_CMDS)
\t$(C120_QHD_INSTALL_TARGET_CMDS)
'''
    subprocess.run(["make", "-s", "-f", "-", str(build / "stamp")], input=makefile,
                   text=True, check=True)
    assert (target / "usr/lib/libc120-qhd.so").stat().st_size > 0
    assert (target / "etc/default/majestic").read_bytes() == (PACKAGE / "majestic.env").read_bytes()
    customizer = REPO / "br-ext-chip-sigmastar/board/tapo-c120/overlay/usr/share/openipc/customizer.sh"
    result = subprocess.run(["sh", "-c", 'cli() { echo "$*"; }; fw_setenv() { :; };\n' +
                             customizer.read_text().split("# The stock crontab")[0]],
                            check=True, capture_output=True, text=True)
    for setting in (".video0.size 2560x1440", ".video0.fps 20", ".video0.bitrate 10000",
                    ".video0.gopSize 40", ".isp.exposure 33", ".motionDetect.visualize false"):
        assert "-s " + setting in result.stdout
    configs = REPO / "br-ext-chip-sigmastar/configs"
    assert "BR2_PACKAGE_C120_QHD=y" in (configs / "ssc377_lite_tp-link-tapo-c120-v1_defconfig").read_text()
    assert "BR2_PACKAGE_C120_QHD=y" not in (configs / "ssc377_lite_defconfig").read_text()
    print("PASS QHD package: compile/install, C120-only selection, 1440p/20fps defaults")


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
        test_ioctl(root / "ioctl")
        test_package(root / "package")
        test_pruning(root / "pruning")
        test_init(root / "init")

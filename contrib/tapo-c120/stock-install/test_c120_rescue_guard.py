"""Run on Linux: python3 tools/test_c120_rescue_guard.py."""
import os
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory() as directory:
    binary = str(Path(directory) / "guard")
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(Path(__file__).with_name("c120_rescue_guard.c")), "-o", binary], check=True)
    fd = os.open("/dev/null", os.O_RDONLY)
    try:
        subprocess.run([binary, "exec", "/bin/sh", "-c", f"test ! -e /proc/self/fd/{fd}"], pass_fds=(fd,), check=True)
    finally:
        os.close(fd)
    pidfile = str(Path(directory) / "pid")
    result = subprocess.run([binary, "watchdog", pidfile], capture_output=True)
    assert result.returncode == 1 and b"no changes made" in result.stderr
    assert not Path(pidfile).exists()
    result = subprocess.run([binary, 'watchdog', pidfile, str(os.getpid())], capture_output=True)
    assert result.returncode == 1 and b'not the stock monitor' in result.stderr
    assert not Path(pidfile).exists()
print("PASS: inherited descriptors closed; missing watchdog fails without changes")

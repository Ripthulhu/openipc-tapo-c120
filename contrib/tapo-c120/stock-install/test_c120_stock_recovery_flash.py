#!/usr/bin/env python3
"""Exercise flash order and rollback without opening a device (Linux host)."""
import os
from itertools import product
from pathlib import Path
import subprocess
import tempfile


source = Path(__file__).with_name("c120_stock_recovery_flash.sh").read_text()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for name, value in (("openipc-raw.bin", b"N"), ("raw-stock-quiesced.bin", b"S")):
        (root / name).write_bytes(value * 0x1000000)
    mock = root / "mock.py"
    mock.write_text('''#!/usr/bin/env python3
import os, sys
from pathlib import Path
p = Path(__file__).parent
a = sys.argv[1:]
if a[0] in ("sync", "sleep"):
    sys.exit(0)
if a[0] == "dd":
    os.execvp("dd", a)
with (p / "calls").open("a") as log:
    log.write(" ".join(a) + "\\n")
if a[0] == "reboot":
    sys.exit(0)
if a[0] == "stock-compare":
    sys.exit(1 if os.environ.get("FAIL_AT") == "compare" and a[2].endswith("openipc-raw.bin") else 0)
counter = p / "counter"
n = int(counter.read_text()) + 1 if counter.exists() else 1
counter.write_text(str(n))
size = int(a[4], 0)
data = sys.stdin.buffer.read()
assert len(data) == size, (len(data), size)
assert len(set(data)) == 1 and data[:1] in (b"N", b"S")
sys.exit(1 if str(n) in os.environ.get("FAIL_AT", "").split(",") else 0)
''')
    mock.chmod(0o700)
    script = root / "flash.sh"
    script.write_text(source.replace("BB=/bin/busybox", f"BB={mock}")
                      .replace("STAGE=/stage", f"STAGE={root}")
                      .replace("WRITER=$STAGE/mtdw-physical", f"WRITER={mock}")
                      .replace("\npreflight\n", "\n# Hardware preflight is tested on the camera.\n"))
    for sensor, (fail_at, expected, reboot) in product(('sc430ai', 'sc438hai'), (
        ("", ["/dev/mtd15", "/dev/mtd15", "/dev/mtd1", "/dev/mtd0"], True),
        ("1", ["/dev/mtd15", "/dev/mtd15"], True),
        ("3", ["/dev/mtd15", "/dev/mtd15", "/dev/mtd1", "/dev/mtd15", "/dev/mtd1"], True),
        ("4", ["/dev/mtd15", "/dev/mtd15", "/dev/mtd1", "/dev/mtd0", "/dev/mtd15", "/dev/mtd1", "/dev/mtd0"], True),
        ("compare", ["/dev/mtd15", "/dev/mtd15", "/dev/mtd1", "/dev/mtd0", "/dev/mtd15", "/dev/mtd1", "/dev/mtd0"], True),
        ("1,2", ["/dev/mtd15", "/dev/mtd15"], False),
    )):
        for name in ("calls", "counter"):
            (root / name).unlink(missing_ok=True)
        result = subprocess.run(["sh", str(script), "--flash", "--yes-i-understand", "test", sensor],
                                env=dict(os.environ, FAIL_AT=fail_at), capture_output=True, text=True)
        calls = (root / "calls").read_text().splitlines()
        writes = [line.split()[1] for line in calls if line.startswith("stock-write-stream ")]
        assert writes == expected, (fail_at, writes, result.stderr)
        assert ("reboot -f" in calls) == reboot, (fail_at, calls)
        assert (result.returncode == 0) == reboot, result.stderr
        if fail_at and reboot:
            assert "STOCK_ROLLBACK_PHYSICAL_VERIFY_OK" in result.stdout
    for args in ([], ["--flash"], ["--flash", "bad-confirmation", "test", "sc430ai"],
                 ["--preflight", "test", "unknown"], ["--flash", "--yes-i-understand", "test"]):
        assert subprocess.run(["sh", str(script), *args], capture_output=True).returncode == 2
print("flash order, partial-write rollback, failed-rollback no-reboot, and argument guards: PASS")

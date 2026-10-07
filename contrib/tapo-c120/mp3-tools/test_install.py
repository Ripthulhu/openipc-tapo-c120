"""Host-only MP3 installer checks; never accesses a camera or room audio."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
script = (HERE / "install.sh").read_text()
FILES = ("usr/bin/lame", "usr/lib/libmp3lame.so.0", "usr/share/licenses/lame/COPYING")


def put(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(0o755)


with tempfile.TemporaryDirectory(prefix="openipc-mp3-test-") as tmp:
    root = Path(tmp)
    package, target, mocks = root / "package", root / "target", root / "mocks"
    for path, body in zip(FILES, ("#!/bin/sh\necho LAME 3.100\n", "library\n", "licence\n")):
        put(package / path, body)
    put(package / "SHA256SUMS", "".join(
        hashlib.sha256((package / p).read_bytes()).hexdigest() + "  " + p + "\n" for p in FILES))
    for name, body in {
        "id": "echo 0", "uname": "echo armv7l",
        "df": "printf 'Filesystem 1024-blocks Used Available Capacity Mounted\\n/dev/fixture 9000 0 9000 0%% /overlay\\n'",
        "sync": ":",
    }.items():
        put(mocks / name, "#!/bin/sh\n" + body + "\n")
    put(target / "lib/ld-musl-armhf.so.1", "runtime\n")
    put(target / "etc/majestic.yaml", "audio:\n  codec: opus\n")
    put(package / "install.sh", script.replace('"/$path', '"' + str(target) + '/$path')
        .replace("/lib/ld-musl-armhf.so.1", str(target / "lib/ld-musl-armhf.so.1"))
        .replace("/usr/bin/lame --version", str(target / "usr/bin/lame") + " --version"))
    env = dict(os.environ, PATH=str(mocks) + os.pathsep + os.environ["PATH"])

    def run(ok=True):
        r = subprocess.run(["sh", str(package / "install.sh")], env=env, capture_output=True, text=True)
        assert (r.returncode == 0) == ok, (r.stdout, r.stderr)
        assert (target / "etc/majestic.yaml").read_text() == "audio:\n  codec: opus\n"
        return r.stdout + r.stderr

    # FAT staging has no executable Unix modes; installation repairs only the tool.
    (package / "usr/bin/lame").chmod(0o644)
    run()
    assert (target / FILES[0]).stat().st_mode & 0o777 == 0o755
    assert (target / FILES[1]).stat().st_mode & 0o777 == 0o644
    hashes = {p: hashlib.sha256((target / p).read_bytes()).hexdigest() for p in FILES}
    run()
    assert hashes == {p: hashlib.sha256((target / p).read_bytes()).hexdigest() for p in FILES}
    put(target / FILES[0], "different tool\n")
    assert "differs" in run(False)
    put(package / FILES[1], "corrupt library\n")
    assert "FAILED" in run(False)
    put(mocks / "uname", "#!/bin/sh\necho x86_64\n")
    assert "ARM" in run(False)
    assert not (target / "etc/init.d").exists()
    assert not list(target.rglob("*.mp3-new"))
    print("PASS MP3 tools: FAT modes, idempotence, hashes, architecture, overwrite refusal, settings/services untouched")

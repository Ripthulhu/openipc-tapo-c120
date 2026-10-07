#!/usr/bin/env python3
"""Exercise the boot-file defects observed on the SC438HAI C120."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "startup", Path(__file__).resolve().parents[1] / "check-rootfs-startup.py"
)
startup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(startup)


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for folder in ("bin", "sbin", "usr/sbin"):
            (self.root / folder).mkdir(parents=True)
        self.write("bin/busybox", b"\x7fELFfixture")
        self.write("init", b"#!/bin/sh\nexit 0\n")
        self.write("usr/sbin/extutils", b"#!/bin/sh\nexit 0\n")
        for name, target in (
            ("bin/sh", "busybox"), ("sbin/init", "../bin/busybox"),
            ("linuxrc", "init"), ("usr/sbin/cli", "extutils"),
        ):
            (self.root / name).symlink_to(target)

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        path.chmod(0o755)

    def test_valid_rootfs(self):
        startup.check(self.root)

    def test_crlf_init(self):
        self.write("init", b"#!/bin/sh\r\nexit 0\r\n")
        with self.assertRaisesRegex(ValueError, "CRLF shebang"):
            startup.check(self.root)

    def test_flattened_links(self):
        for name, target in (("linuxrc", b"init"), ("usr/sbin/cli", b"extutils")):
            with self.subTest(name=name):
                path = self.root / name
                original = path.readlink()
                path.unlink()
                self.write(name, target)
                with self.assertRaisesRegex(ValueError, "flattened symlink"):
                    startup.check(self.root)
                path.unlink()
                path.symlink_to(original)

    def test_missing_interpreter(self):
        self.write("init", b"#!/missing/sh\n")
        with self.assertRaisesRegex(ValueError, "missing interpreter"):
            startup.check(self.root)

    def test_absolute_link_stays_in_image(self):
        path = self.root / "bin/sh"
        path.unlink()
        path.symlink_to("/bin/busybox")
        self.assertEqual(startup.image_path(self.root, "/bin/sh"), self.root / "bin/busybox")
        startup.check(self.root)

    def test_empty_shebang(self):
        self.write("init", b"#!\n")
        with self.assertRaisesRegex(ValueError, "empty shebang"):
            startup.check(self.root)


if __name__ == "__main__":
    unittest.main()

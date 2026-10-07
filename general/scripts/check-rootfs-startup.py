#!/usr/bin/env python3
"""Reject startup files that Linux cannot execute before packing a rootfs."""
import os
from pathlib import Path
import sys


def image_path(root, name):
    pending = name.lstrip("/").split("/")
    resolved = []
    links = 0
    while pending:
        item = pending.pop(0)
        if item in ("", "."):
            continue
        if item == "..":
            if resolved:
                resolved.pop()
            continue
        path = root.joinpath(*resolved, item)
        if path.is_symlink():
            links += 1
            if links > 40:
                raise ValueError(f"symlink loop: {name}")
            target = os.readlink(path)
            if target.startswith("/"):
                resolved = []
            pending = target.split("/") + pending
        else:
            resolved.append(item)
    return root.joinpath(*resolved)


def check(root):
    for name in ("/init", "/bin/sh", "/sbin/init", "/linuxrc", "/usr/sbin/cli"):
        path = image_path(root, name)
        if not path.is_file() or not path.stat().st_mode & 0o111:
            raise ValueError(f"missing executable: {name}")
        with path.open("rb") as file:
            header = file.read(256)
        if not header.startswith((b"\x7fELF", b"#!")):
            raise ValueError(f"invalid executable or flattened symlink: {name}")
    scripts = 0
    for folder, _, names in os.walk(root, followlinks=False):
        for name in names:
            path = Path(folder) / name
            if path.is_symlink():
                continue
            with path.open("rb") as file:
                if file.read(2) != b"#!":
                    continue
                line = file.readline()
            if b"\r" in line:
                raise ValueError(f"CRLF shebang: {path.relative_to(root)}")
            words = line.split(None, 1)
            if not words:
                raise ValueError(f"empty shebang: {path.relative_to(root)}")
            interpreter = words[0].decode("utf-8")
            executable = image_path(root, interpreter)
            if not interpreter.startswith("/") or not executable.is_file() or not executable.stat().st_mode & 0o111:
                raise ValueError(f"missing interpreter {interpreter}: {path.relative_to(root)}")
            scripts += 1
    print(f"rootfs startup: required executables and {scripts} script interpreters OK")


if __name__ == "__main__":
    try:
        check(Path(sys.argv[1]))
    except (OSError, ValueError) as error:
        sys.exit(f"rootfs startup: {error}")

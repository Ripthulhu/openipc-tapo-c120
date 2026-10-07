#!/usr/bin/env python3
"""Serve public bootstrap files read-only to one camera."""

import argparse
import tempfile
from pathlib import Path

import tftpy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--bind", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", type=int, default=1069)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    if not root.is_dir():
        parser.error("root must be a directory")
    if not 1024 <= args.port <= 65535:
        parser.error("port must be 1024-65535")

    def read_file(name, raddress, rport):
        if raddress != args.client or Path(name).name != name or "\\" in name:
            return None
        path = root / name
        return path.open("rb") if path.is_file() and not path.is_symlink() else None

    with tempfile.TemporaryDirectory() as empty_root:
        server = tftpy.TftpServer(
            tftproot=empty_root,
            dyn_file_func=read_file,
            upload_open=lambda path, context: None,
        )
        print(f"Serving {root} to {args.client} on {args.bind}:{args.port}", flush=True)
        server.listen(args.bind, args.port)


if __name__ == "__main__":
    main()

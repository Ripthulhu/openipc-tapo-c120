#!/usr/bin/env python3
"""Serve staged files to one camera, optionally receiving one named backup."""

import argparse
import tempfile
from pathlib import Path

import tftpy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--bind", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--receive", type=Path, help="accept this basename into a new local file")
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    if not root.is_dir():
        parser.error("root must be a directory")
    if args.receive and (args.receive.exists() or not args.receive.parent.is_dir()):
        parser.error("receive file must be new and its parent must exist")

    def read_file(name, raddress, rport):
        if raddress != args.client or Path(name).name != name or "\\" in name:
            return None
        path = root / name
        return path.open("rb") if path.is_file() else None

    def receive_file(path, context):
        if (not args.receive or context.host != args.client or
                context.file_to_transfer != args.receive.name):
            return None
        return args.receive.open("xb")

    with tempfile.TemporaryDirectory() as empty_root:
        server = tftpy.TftpServer(
            tftproot=empty_root,
            dyn_file_func=read_file,
            upload_open=receive_file,
        )
        print(f"Serving {root} to {args.client} on {args.bind}:69", flush=True)
        server.listen(args.bind, 69)


if __name__ == "__main__":
    main()

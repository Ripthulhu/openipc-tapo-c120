"""Loopback-only transfer check; optionally exercise the dumped stock BusyBox."""
import os
import io
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

import tftpy

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    public = root / 'public'
    public.mkdir()
    payload = bytes(range(256)) * 64
    (public / 'probe').write_bytes(payload)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    with (root / 'server.log').open('wb') as log:
        server = subprocess.Popen([sys.executable, str(Path(__file__).with_name('c120_tftp_serve.py')),
            '--root', str(public), '--bind', '127.0.0.1', '--client', '127.0.0.1', '--port', str(port)],
            stdout=log, stderr=log)
        try:
            time.sleep(1)
            assert server.poll() is None
            output = root / 'received'
            stock_root = os.environ.get('C120_STOCK_ROOT')
            if stock_root:
                subprocess.run(['qemu-arm', '-L', stock_root, stock_root + '/bin/busybox',
                    'tftp', '-g', '-r', 'probe', '-l', str(output), '127.0.0.1', str(port)],
                    check=True, timeout=15)
            else:
                tftpy.TftpClient('127.0.0.1', port).download('probe', str(output), timeout=2, retries=2)
            assert output.read_bytes() == payload
            for name in ('../probe', '/probe', 'missing'):
                try:
                    tftpy.TftpClient('127.0.0.1', port).download(name, io.BytesIO(), timeout=1, retries=1)
                except tftpy.TftpException:
                    pass
                else:
                    raise AssertionError('Unexpected transfer: ' + name)
            try:
                tftpy.TftpClient('127.0.0.1', port).upload('written', str(output), timeout=1, retries=1)
            except tftpy.TftpException:
                pass
            else:
                raise AssertionError('Write accepted')
            assert not (public / 'written').exists()
        finally:
            server.terminate()
            server.wait(timeout=10)
print('PASS: unprivileged TFTP, byte-exact transfer, traversal/missing-file/upload rejection')

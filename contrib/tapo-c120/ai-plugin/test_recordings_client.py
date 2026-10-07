"""A cut-off media response must not advance the recordings cursor."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading


clip_id = '0123456789abcdef-00000000000000000001'
video = b'complete synthetic recording'


class Handler(BaseHTTPRequestHandler):
    truncated = True

    def do_GET(self):
        if self.path.startswith('/cgi-bin/c120-recordings-api.cgi?'):
            body = json.dumps(dict(schemaVersion=1, cameraId='test-camera', nextCursor='next',
                hasMore=False, recordings=[dict(id=clip_id, bytes=len(video),
                    videoUrl='/video.mp4', snapshotUrl=None, detections=[])])).encode()
        elif self.path == '/video.mp4':
            body = video[:3] if self.truncated else video
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(video) if self.path == '/video.mp4' else len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def log_message(self, *args):
        pass


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            command = [sys.executable, str(Path(__file__).with_name('recordings-client.py')),
                f'http://127.0.0.1:{server.server_port}', str(output)]
            env = {**os.environ, 'CAMERA_PASSWORD': 'test'}
            failed = subprocess.run(command, env=env, capture_output=True, text=True)
            assert failed.returncode != 0, failed.stdout
            assert not (output / clip_id / 'video.mp4').exists()
            assert not (output / 'cursor.json').exists()
            Handler.truncated = False
            succeeded = subprocess.run(command, env=env, capture_output=True, text=True)
            assert succeeded.returncode == 0, succeeded.stderr
            assert (output / clip_id / 'video.mp4').read_bytes() == video
            assert json.loads((output / 'cursor.json').read_text())['cursor'] == 'next'
    finally:
        server.shutdown()
        server.server_close()
    print('PASS recordings client rejects truncated media and retries safely')


if __name__ == '__main__':
    main()

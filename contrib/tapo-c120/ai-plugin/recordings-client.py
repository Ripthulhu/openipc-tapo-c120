#!/usr/bin/env python3
"""Fetch newly completed recordings using only Python's standard library."""
import argparse
import getpass
import json
import os
from pathlib import Path
import re
import shutil
import urllib.error
import urllib.parse
import urllib.request


def atomic_json(path, value):
    temporary = path.with_suffix('.tmp')
    with temporary.open('w', encoding='utf-8') as out:
        json.dump(value, out, indent=2)
        out.write('\n')
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('camera', help='Camera origin, preferably local HTTPS')
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    origin = args.camera.rstrip('/')
    parsed = urllib.parse.urlsplit(origin)
    if parsed.scheme not in ('http', 'https') or not parsed.netloc or parsed.username or parsed.query or parsed.fragment or parsed.path:
        parser.error('camera must be an http(s) origin without credentials or a path')
    manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    manager.add_password(None, origin, 'root', os.environ.get('CAMERA_PASSWORD') or getpass.getpass('Camera root password: '))

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            raise RuntimeError('Unexpected redirect; use the final camera HTTPS origin')

    opener = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(manager),
        urllib.request.HTTPBasicAuthHandler(manager), NoRedirect())
    args.directory.mkdir(parents=True, exist_ok=True)
    checkpoint = args.directory / 'cursor.json'
    saved = json.loads(checkpoint.read_text()) if checkpoint.exists() else {}
    if saved and saved.get('origin') != origin:
        raise RuntimeError('Use a separate output directory for each camera origin')
    cursor = saved.get('cursor', '')

    def download(relative, destination, expected_bytes=None):
        if not relative.startswith('/') or relative.startswith('//'):
            raise ValueError('Camera returned a non-relative media URL')
        url = urllib.parse.urljoin(origin, relative)
        if urllib.parse.urlsplit(url).netloc != parsed.netloc:
            raise ValueError('Camera returned a different media origin')
        part = destination.with_suffix(destination.suffix + '.partial')
        with opener.open(url, timeout=30) as response, part.open('wb') as out:
            shutil.copyfileobj(response, out, length=65536)
            size = out.tell()
            declared = response.headers.get('Content-Length')
            if (expected_bytes is not None and size != expected_bytes) or (declared is not None and size != int(declared)):
                raise IOError(f'Incomplete download: {relative} ({size} bytes)')
            out.flush()
            os.fsync(out.fileno())
        os.replace(part, destination)

    while True:
        url = origin + '/cgi-bin/c120-recordings-api.cgi?' + urllib.parse.urlencode({'cursor': cursor} if cursor else {})
        try:
            with opener.open(url, timeout=30) as response:
                page = json.load(response)
        except urllib.error.HTTPError as exc:
            if exc.code == 410:
                raise RuntimeError('Storage generation changed. Review existing downloads before removing cursor.json for a fresh scan.') from exc
            raise
        if page['schemaVersion'] != 1:
            raise RuntimeError('Unsupported recordings API schema')
        for clip in page['recordings']:
            if not re.fullmatch(r'[0-9a-f]{16}-[0-9]{20}', clip['id']):
                raise ValueError('Invalid clip ID')
            folder = args.directory / clip['id']
            folder.mkdir(exist_ok=True)
            if (folder / 'metadata.json').exists():
                continue
            try:
                download(clip['videoUrl'], folder / 'video.mp4', clip['bytes'])
            except urllib.error.HTTPError as exc:
                if exc.code != 404:
                    raise
                print(f"Expired before download: {clip['id']}")
                continue
            if clip['snapshotUrl']:
                try:
                    download(clip['snapshotUrl'], folder / 'snapshot.jpg')
                except urllib.error.HTTPError as exc:
                    if exc.code != 404:
                        raise
                    print(f"Still expired before download: {clip['id']}")
            atomic_json(folder / 'metadata.json', clip)
            print(clip['id'], ', '.join(item['category'] for item in clip['detections']) or 'unclassified')
        cursor = page['nextCursor']
        atomic_json(checkpoint, {'origin': origin, 'cameraId': page['cameraId'], 'cursor': cursor})
        if not page['hasMore']:
            break


if __name__ == '__main__':
    main()

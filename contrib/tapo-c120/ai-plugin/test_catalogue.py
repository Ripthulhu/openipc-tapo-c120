"""Synthetic storage/API/outbox tests. No devices, credentials or room media."""
import ctypes as C
from ctypes.util import find_library
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

BASE = Path(__file__).resolve().parent


def main():
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            requests.append((self.path, json.loads(self.rfile.read(int(self.headers['Content-Length'])))))
            if self.path == '/slow':
                time.sleep(4)
            self.send_response(500 if self.path == '/fail' else 302 if self.path == '/redirect' else 204)
            if self.path == '/redirect':
                self.send_header('Location', '/unexpected')
            self.send_header('Content-Length', '0')
            self.end_headers()

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    endpoint = f'http://127.0.0.1:{server.server_port}'
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        shim = root / 'storage.c'
        shim.write_text(r'''
#define _GNU_SOURCE
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
static int full;
static unsigned metadata_reads;
unsigned test_reads(void){unsigned n=metadata_reads;metadata_reads=0;return n;}
int test_openat(int fd,const char *path,int flags,...){
    mode_t mode=0;
    if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
    if(strlen(path)==25 && strspn(path,"0123456789")==20 && !strcmp(path+20,".json") && (flags&O_ACCMODE)==O_RDONLY)++metadata_reads;
    return openat(fd,path,flags,mode);
}
void test_full(int value){full=value;}
int test_fstatfs(int fd,struct statfs *s){int r=syscall(SYS_fstatfs,fd,s);if(!r){s->f_type=0xef53;memset(&s->f_fsid,42,sizeof(s->f_fsid));}return r;}
int test_fstatvfs(int fd,struct statvfs *s){int r=fstatvfs(fd,s);if(!r && full){s->f_blocks=100000;s->f_frsize=4096;s->f_bavail=0;}return r;}
''')
        flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'json-c', 'libcurl'], text=True).split()
        # The shim itself calls real fstatvfs; compile it without the macro.
        subprocess.run(['gcc', '-fPIC', '-c', str(shim), '-o', str(root / 'shim.o')], check=True)
        libpath = root / 'catalogue.so'
        subprocess.run(['gcc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Dfstatfs=test_fstatfs', '-Dfstatvfs=test_fstatvfs', '-Dopenat=test_openat',
                        str(BASE / 'catalogue.c'), str(BASE / 'notify.c'), str(root / 'shim.o'),
                        '-o', str(libpath), *flags], check=True)
        lib = C.CDLL(str(libpath))
        js = C.CDLL(find_library('json-c'))
        js.json_tokener_parse.argtypes = [C.c_char_p]
        js.json_tokener_parse.restype = C.c_void_p
        js.json_object_put.argtypes = [C.c_void_p]
        js.json_object_to_json_string_ext.argtypes = [C.c_void_p, C.c_int]
        js.json_object_to_json_string_ext.restype = C.c_char_p
        for name in ('catalogue_configure', 'catalogue_storage', 'catalogue_valid'):
            getattr(lib, name).argtypes = [C.c_void_p]
        lib.catalogue_complete.argtypes = [C.c_char_p, C.c_char_p, C.c_double, C.c_double, C.c_void_p, C.c_char_p, C.c_double, C.c_int]
        lib.catalogue_query.argtypes = [C.c_char_p, C.POINTER(C.c_int)]
        lib.catalogue_query.restype = C.c_void_p
        lib.catalogue_native.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p]

        def call(name, value):
            obj = js.json_tokener_parse(json.dumps(value).encode())
            try:
                return getattr(lib, name)(obj)
            finally:
                js.json_object_put(obj)

        def query(params='', code=200):
            status = C.c_int()
            obj = lib.catalogue_query(params.encode(), C.byref(status))
            try:
                result = json.loads(js.json_object_to_json_string_ext(obj, 0))
            finally:
                js.json_object_put(obj)
            assert status.value == code, (status.value, code, result)
            return result

        video = subprocess.check_output(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
            'color=size=256x144:rate=30', '-t', '1', '-an', '-c:v', 'libx264', '-movflags',
            'frag_keyframe+empty_moov+default_base_moof', '-f', 'mp4', 'pipe:1'])
        media = root / 'card'
        media.mkdir()
        cfg = dict(enabled=False, url='', token='')
        call('catalogue_configure', cfg)
        call('catalogue_storage', dict(path=str(media / '%F'), maxUsage=95, enabled=False))
        query(code=503)
        lib.catalogue_work()
        empty = query()
        assert empty['recordings'] == [] and not empty['hasMore']
        generation = empty['storageGeneration']
        cat = media / '.c120-recordings'

        def scan():
            state = json.loads((cat / 'state.json').read_text())
            state['lastScan'] = 0
            (cat / 'state.json').write_text(json.dumps(state))
            lib.catalogue_work()

        def register(name, label=None, notify=True):
            path = media / name
            path.parent.mkdir(exist_ok=True, parents=True)
            path.write_bytes(video)
            summary = [] if label is None else [dict(category=label, type='object', model='stock',
                maxConfidence=.9, firstSeen='2026-10-07T12:00:00Z', lastSeen='2026-10-07T12:00:02Z')]
            obj = js.json_tokener_parse(json.dumps(summary).encode())
            try:
                assert lib.catalogue_complete(os.fsencode(path), b'ai' if label else b'native',
                    time.time()-2, 2, obj, None, 0, notify) == 0
            finally:
                js.json_object_put(obj)
            return path

        first = register('2026-10-07/a space & one.mp4', 'pet')
        second = register('2026-10-07/two.mp4', 'person')
        third = register('2026-10-07/three.mp4')
        lib.catalogue_work()
        lib.test_reads()
        lib.catalogue_work()
        assert lib.test_reads() == 0, 'idle worker reopened completed recording metadata'
        page = query('limit=1')
        one = page['recordings'][0]
        assert page['hasMore'] and one['detections'][0]['category'] == 'pet'
        assert '%20%26%20' in one['videoUrl'] and one['snapshotUrl'] is None
        cursor = page['nextCursor']
        page2 = query('limit=1&cursor=' + cursor)
        assert page2['recordings'][0]['id'] != one['id'] and page2['hasMore']
        assert len(query('cursor=' + page2['nextCursor'])['recordings']) == 1
        assert query('id=' + one['id'])['id'] == one['id']
        assert len(query('category=pet')['recordings']) == 1
        assert len(query('source=native')['recordings']) == 1
        assert not query('source=manual')['recordings']
        assert not query('after=2099-01-01T00%3A00%3A00Z')['recordings']
        for args in ('limit=0', 'limit=201', 'limit=1&limit=2', 'wat=1', 'id=../../etc/shadow',
                     'id=' + one['id'] + '&limit=1', 'category=pe', 'source=cat',
                     'after=oops', 'limit=1%00', 'cursor=bad%'):
            query(args, 400)
        query('cursor=0000000000000000:1', 410)
        query('cursor=' + generation + ':99999', 400)
        query('id=' + generation + '-00000000000000099999', 404)
        # Repeated native close hooks are idempotent; no replay on an imported historical clip.
        assert lib.catalogue_native(os.fsencode(third), b'stop', b'2') == 0
        assert len(query()['recordings']) == 3
        assert lib.catalogue_native(os.fsencode(third), b'error', b'2') != 0

        # A crash after metadata but before its path ref is repaired without a second ID.
        latest = query()['recordings'][-1]['id']
        saved_state = json.loads((cat / 'state.json').read_text())
        saved_state['indexed'] = 2
        (cat / 'state.json').write_text(json.dumps(saved_state))
        for ref in cat.glob('p-*.json'):
            if json.loads(ref.read_text()) == 3:
                ref.unlink()
        lib.catalogue_work()
        assert query()['recordings'][-1]['id'] == latest
        assert len(query()['recordings']) == 3
        assert any(json.loads(ref.read_text()) == 3 for ref in cat.glob('p-*.json'))
        # Legacy index files are ignored; sidecars remain the source of page order.
        (cat / 'index').write_text('broken legacy index')
        assert len(query()['recordings']) == 3

        # Import only closed MP4s, never symlinks, incomplete files or open native writers.
        old = media / 'old.mp4'; old.write_bytes(video); os.utime(old, (1, 1))
        active = media / 'active.mp4'; active.write_bytes(video); os.utime(active, (2, 2))
        outside = root / 'outside.mp4'; outside.write_bytes(video)
        (media / 'escape.mp4').symlink_to(outside)
        (media / 'bad.mp4').write_bytes(b'not an mp4')
        (media / 'pending.partial').write_bytes(video)
        state = json.loads((cat / 'state.json').read_text())
        state['lastScan'] = int(time.time())-61
        (cat / 'state.json').write_text(json.dumps(state))
        lib.catalogue_work()
        assert len(query()['recordings']) == 3, 'recovery scan still runs every minute'
        with active.open('ab'):
            scan()
            clips = query()['recordings']
            assert len(clips) == 4 and clips[-1]['source'] == 'imported'
            assert clips[-1]['durationSeconds'] is None and not clips[-1]['detections']
        assert lib.catalogue_complete(os.fsencode(media / 'escape.mp4'), b'ai', 1, 1, None, None, 0, 1) != 0
        # A symlink introduced after cataloguing must not be offered for download.
        first.unlink(); first.symlink_to(outside)
        query('id=' + one['id'], 404)
        lib.catalogue_work()
        assert all(clip['id'] != one['id'] for clip in query()['recordings'])
        assert outside.exists()

        # Persistent webhook attempts, stable IDs, no redirect, config cancellation and TTL.
        hook = dict(enabled=True, url=endpoint + '/fail', token='')
        assert call('catalogue_valid', hook)
        for change in (dict(url='file:///etc/passwd'), dict(url='http://a:b@host/'), dict(token='bad\r\n'), dict(enabled='yes')):
            assert not call('catalogue_valid', {**hook, **change})
        call('catalogue_configure', hook)
        register('hook.mp4', 'pet')
        pending_path = sorted(cat.glob('[0-9]*.json'))[-1]
        good = pending_path.read_bytes()
        pending_path.write_bytes(b'{broken')
        lib.catalogue_work()
        assert not requests, 'corrupt metadata was delivered'
        pending_path.write_bytes(good)
        lib.catalogue_work(); assert requests[-1][0] == '/fail'
        event = requests[-1][1]
        assert event['type'] == 'recording.ready' and event['recordingId'] == event['recording']['id']
        item = cat / (event['recordingId'].split('-')[-1] + '.json')
        saved = json.loads(item.read_text()); assert saved['attempts'] == 1 and saved['delivery'] == 'pending'
        lib.catalogue_work(); assert len(requests) == 1, 'backoff ignored'
        saved['nextAttempt'] = 0; item.write_text(json.dumps(saved))
        lib.catalogue_work(); assert len(requests) == 2 and requests[-1][1]['recordingId'] == event['recordingId']
        call('catalogue_configure', {**hook, 'url': endpoint + '/ok'})
        lib.catalogue_work(); assert len(requests) == 2 and json.loads(item.read_text())['delivery'] == 'cancelled'
        register('delivered.mp4', 'person'); lib.catalogue_work()
        assert requests[-1][0] == '/ok'
        for path in ('/redirect', '/slow'):
            call('catalogue_configure', {**hook, 'url': endpoint + path})
            register(path[1:] + '.mp4', 'person')
            started = time.monotonic(); lib.catalogue_work()
            assert time.monotonic()-started < 3.8
        assert not any(path == '/unexpected' for path, _ in requests)
        pending = [p for p in cat.glob('[0-9]*.json') if json.loads(p.read_text()).get('delivery') == 'pending']
        assert pending
        for p in pending:
            value = json.loads(p.read_text()); value['completedEpoch'] = 1; value['nextAttempt'] = 0
            p.write_text(json.dumps(value))
        before = len(requests); lib.catalogue_work(); assert len(requests) == before

        # Full-card AI retention deletes only closed catalogued recordings, including native/imported.
        lib.catalogue_recording_enabled(1)
        lib.test_full(1)
        protected = media / 'active.mp4'
        with protected.open('ab'):
            lib.catalogue_work()
            assert protected.exists() and outside.exists() and (media / 'pending.partial').exists()
        assert not second.exists() and not third.exists() and not old.exists()
        assert query('cursor=' + cursor)['storageGeneration'] == generation
        lib.test_full(0)
        # Existing native retention is authoritative when the native recorder is enabled.
        kept = register('keep-native.mp4')
        call('catalogue_storage', dict(path=str(media), enabled=True, maxUsage=95))
        lib.test_full(1); lib.catalogue_work(); assert kept.exists(); lib.test_full(0)
        # A different card produces a new generation, never silently accepts the old cursor.
        other = root / 'card2'; other.mkdir()
        call('catalogue_storage', dict(path=str(other), enabled=False))
        lib.catalogue_work(); query('cursor=' + cursor, 410)
        symlink = root / 'link'; symlink.symlink_to(other, target_is_directory=True)
        call('catalogue_storage', dict(path=str(symlink), enabled=False))
        query(code=503)
    server.shutdown(); server.server_close()
    print('PASS: catalogue IDs, cursor pages/filters/errors, historical import, native hooks, transaction repair')
    print('PASS: path confinement, symlinks, open files, deletion cleanup, SD generation, persistent webhook retries/expiry')
    print('PASS: full-card AI retention, protected open/partial files, native-recorder ownership, cursor continuity')
    print('PASS: idle metadata reads eliminated, delayed recovery scan, corrupt pending metadata remains retryable')


if __name__ == '__main__':
    main()

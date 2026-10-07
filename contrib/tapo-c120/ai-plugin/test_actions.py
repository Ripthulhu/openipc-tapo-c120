"""Host checks with real libcurl and synthetic HTTP/video only. No camera room recordings."""
import ctypes as C
from ctypes.util import find_library
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import shlex
import tempfile
import threading
import time

BASE = Path(__file__).resolve().parent


def main():
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers['Content-Length']))
            requests.append((self.path,body,dict(self.headers)))
            if self.path == '/slow': time.sleep(4)
            self.send_response(500 if self.path == '/fail' else 302 if self.path == '/redirect' else 200)
            if self.path == '/redirect': self.send_header('Location','/unexpected')
            self.send_header('Content-Length','0'); self.end_headers()

        def do_GET(self):
            self.send_response(200); self.end_headers()
            try:
                while True:
                    self.wfile.write(video); self.wfile.flush(); time.sleep(.1)
            except (BrokenPipeError,ConnectionResetError): pass

        def log_message(self,*args): pass

    server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    url = 'http://127.0.0.1:'+str(server.server_port)
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        flags = shlex.split(os.environ['HOST_ACTION_CFLAGS']) if 'HOST_ACTION_CFLAGS' in os.environ else subprocess.check_output(['pkg-config','--cflags','--libs','json-c','libcurl'],text=True).split()
        binary = root/'actions.so'
        clock = root/'clock.c'
        clock.write_text('#include <time.h>\nstatic double seconds=100;\nvoid test_time(double t){seconds=t;}\nint test_clock_gettime(clockid_t id,struct timespec *t){(void)id;t->tv_sec=(time_t)seconds;t->tv_nsec=(long)((seconds-t->tv_sec)*1e9);return 0;}\n')
        subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        '-Dclock_gettime=test_clock_gettime',str(clock),str(BASE/'notify.c'),str(BASE/'record.c'),
                        '-o',str(binary),*flags],check=True)
        lib = C.CDLL(str(binary)); js = C.CDLL(find_library('json-c'))
        lib.test_time.argtypes=[C.c_double]
        lib.notify_observe.argtypes=[C.c_char_p]
        js.json_tokener_parse.argtypes=[C.c_char_p]; js.json_tokener_parse.restype=C.c_void_p
        js.json_object_put.argtypes=[C.c_void_p]
        js.json_object_to_json_string_ext.argtypes=[C.c_void_p,C.c_int]; js.json_object_to_json_string_ext.restype=C.c_char_p
        for name in ['notify_configure','notify_event','notify_valid','record_configure','record_storage','record_valid']:
            getattr(lib,name).argtypes=[C.c_void_p]
        for name in ['notify_state','record_state']: getattr(lib,name).restype=C.c_void_p

        def call(name,value):
            o = js.json_tokener_parse(json.dumps(value).encode()); assert o
            try: return getattr(lib,name)(o)
            finally: js.json_object_put(o)

        def state(name):
            o = getattr(lib,name)()
            try: return json.loads(js.json_object_to_json_string_ext(o,0))
            finally: js.json_object_put(o)

        def drain(seconds=4):
            end = time.monotonic()+seconds
            while time.monotonic()<end:
                started=time.monotonic(); lib.notify_poll()
                assert time.monotonic()-started < .15, 'notification delivery blocked inference loop'
                if not state('notify_state')['queued']: break
                time.sleep(.01)
            assert not state('notify_state')['queued']

        cfg = dict(enabled=True,format='webhook',url=url+'/ok',token='',cooldownSeconds=60,categories=['person','pet'])
        assert call('notify_valid',cfg)
        assert call('notify_valid',{**cfg,'categories':['bird']})
        for change in [dict(url='file:///etc/passwd'),dict(url='http://user:pw@host'),dict(url=url+'/#fragment'),dict(token='bad\nheader'),dict(cooldownSeconds=4294967356),dict(categories=['pet','pet']),dict(categories=['cat'])]:
            assert not call('notify_valid',{**cfg,**change})
        call('notify_configure',cfg)
        for label in ['person','person','pet','vehicle']:
            lib.notify_observe(label.encode())
            call('notify_event',dict(label=label,type='object',confidence=.9,id=1,time=1,bootId='test'))
        drain(); assert len(requests)==2
        assert state('notify_state')['delivered']==2
        assert json.loads(requests[0][1])['event']['label']=='person'
        # Confidence flicker must not turn a lingering pet into fresh notifications.
        def arrival(label):
            lib.notify_observe(label.encode())
            call('notify_event',dict(label=label,type='object',confidence=.7,id=10,time=1,bootId='test'))
            drain()
        for stamp in range(101,322,20):
            lib.test_time(stamp); arrival('pet')
        assert state('notify_state')['delivered']==2 and state('notify_state')['suppressed']>=11
        lib.test_time(322); arrival('person')
        assert state('notify_state')['delivered']==3, 'pet presence suppressed an independent person arrival'
        lib.test_time(340); arrival('pet')
        lib.test_time(399); arrival('pet')
        assert state('notify_state')['delivered']==3, 'a gap shorter than the cooldown re-armed pet alerts'
        lib.test_time(459); arrival('pet')
        assert state('notify_state')['delivered']==4 and json.loads(requests[-1][1])['event']['label']=='pet'
        call('notify_configure',cfg)
        lib.test_time(500); arrival('pet')
        assert state('notify_state')['delivered']==4, 'saving settings reset the ongoing pet episode'
        # Sounds retain their independent minimum notification interval, not visual presence.
        call('notify_configure',{**cfg,'categories':['bark']})
        for stamp in (500,501,560):
            lib.test_time(stamp)
            call('notify_event',dict(label='bark',type='sound',confidence=.9,id=11,time=1,bootId='test')); drain()
        assert state('notify_state')['delivered']==6
        # Model warmup primes an independent bird episode without an arrival storm.
        lib.notify_prime.argtypes=[C.c_char_p]
        call('notify_configure',{**cfg,'categories':['bird','pet']})
        lib.test_time(600); lib.notify_prime(b'bird'); arrival('bird')
        lib.test_time(659); arrival('bird'); assert state('notify_state')['delivered']==6
        lib.test_time(719); arrival('bird'); assert state('notify_state')['delivered']==7
        call('notify_configure',cfg)
        before_errors=len(requests)
        # Failed/slow/redirect requests never block or retry. A test bypasses category cooldown only.
        for path in ['/fail','/slow','/redirect']:
            call('notify_configure',{**cfg,'url':url+path})
            call('notify_event',dict(label='test',type='test',confidence=1,id=2,time=1,bootId='test'))
            drain()
        assert state('notify_state')['failed']==3 and len(requests)==before_errors+3
        call('notify_configure',{**cfg,'format':'ntfy','url':url+'/ntfy','token':'test-token'})
        call('notify_event',dict(label='test',type='test',confidence=1,id=3,time=1,bootId='test')); drain()
        assert requests[-1][2]['Authorization']=='Bearer test-token' and not requests[-1][1].startswith(b'{')
        call('notify_configure',{**cfg,'enabled':False})
        call('notify_event',dict(label='test',type='test')); lib.notify_poll(); assert not state('notify_state')['queued']
        call('notify_configure',cfg)
        for _ in range(12): call('notify_event',dict(label='test',type='test',confidence=1,id=3,time=1,bootId='test'))
        assert state('notify_state')['queued']==8 and state('notify_state')['dropped']==4
        lib.notify_stop()
        rec = dict(enabled=True,seconds=1,categories=['person'])
        assert call('record_valid',rec)
        assert call('record_valid',{**rec,'categories':['bird']})
        for change in [dict(seconds=0),dict(seconds=601),dict(categories=['cat']),dict(categories=['person','person'])]:
            assert not call('record_valid',{**rec,**change})
        call('record_configure',rec)
        call('record_storage',dict(path=str(root/'refused'),maxUsage=95,enabled=False))
        lib.record_observe.argtypes=[C.c_char_p]; lib.record_observe(b'person'); lib.record_poll()
        assert state('record_state')['status']=='Waiting for writable recording storage'
        assert not (root/'refused').exists(), 'recorder wrote to RAM/root filesystem'
        call('record_storage',dict(path=str(root),enabled=True)); lib.record_poll()
        assert state('record_state')['status']=='Paused: native recorder enabled'
        lib.record_stop()
        # The test-only filesystem shim permits a synthetic stream under temp; production still refuses it.
        shim = root/'storage.c'
        shim.write_text('#include <sys/statfs.h>\n#include <sys/syscall.h>\n#include <unistd.h>\n#include <string.h>\nint test_statfs(const char *p,struct statfs *s){int r=syscall(SYS_statfs,p,s);if(!r){s->f_type=0xef53;if(strcmp(p,"/"))memset(&s->f_fsid,42,sizeof(s->f_fsid));}return r;}\n')
        positive = root/'record.so'
        subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror','-Dstatfs=test_statfs',f'-DC120_RECORD_URL="{url}/video"',str(BASE/'record.c'),str(shim),'-o',str(positive),*flags],check=True)
        video = subprocess.check_output(['ffmpeg','-v','error','-f','lavfi','-i','color=size=256x144:rate=30','-t','1','-an','-c:v','libx264','-movflags','frag_keyframe+empty_moov+default_base_moof','-f','mp4','pipe:1'])
        old = lib; lib = C.CDLL(str(positive))
        for name in ['record_configure','record_storage']: getattr(lib,name).argtypes=[C.c_void_p]
        lib.record_state.restype=C.c_void_p; lib.record_observe.argtypes=[C.c_char_p]
        call('record_configure',rec); call('record_storage',dict(path=str(root/'recordings'),maxUsage=99,enabled=False))
        lib.record_observe(b'vehicle'); lib.record_poll(); assert not state('record_state')['active']
        lib.record_observe(b'person')
        end=time.monotonic()+2
        while time.monotonic()<end: lib.record_poll(); time.sleep(.01)
        assert state('record_state')['clips']==1 and state('record_state')['errors']==0
        files=list((root/'recordings').glob('*.mp4')); assert len(files)==1 and not list((root/'recordings').glob('*.partial'))
        probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(files[0])]))
        assert probe['streams'][0]['codec_name']=='h264'
        lib.record_stop(); old.notify_stop()
    server.shutdown(); server.server_close()
    print('PASS: notification validation, filters, cooldown, bounded queue, errors, slow server, no redirects, ntfy, disabled mode')
    print('PASS: lingering pet/flicker suppression, quiet-period re-arm, independent categories, settings reload and sound cooldown')
    print('PASS: recording validation, category gate, no flash/RAM fallback, native-recorder conflict, timed synthetic playable MP4')


if __name__=='__main__': main()

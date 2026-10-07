"""Exercise post-load readiness against synthetic local HTTP; no camera or IPU calls."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

BASE = Path(__file__).resolve().parent
CASES = [('transient', True, 2), ('timeout', False, 3), ('http-error', False, 1),
         ('malformed', False, 1), ('substream', False, 1), ('roi', False, 1),
         ('owner-change', False, 1), ('stopping', False, 1), ('setup-ap', False, 1),
         ('owner-before', False, 0)]
RUNTIME = [('runtime-timeout', True, 1), ('runtime-expired', False, 1),
           ('runtime-owner', False, 1), ('runtime-stopping', False, 1),
           ('runtime-ap', False, 1), ('runtime-no-device', False, 1),
           ('runtime-cleanup', False, 1), ('runtime-never-verified', False, 1),
           ('runtime-substream', False, 1), ('runtime-malformed', False, 1),
           ('runtime-http-error', False, 1), ('runtime-invalid-then-timeout', False, 2),
           ('runtime-malformed-then-timeout', False, 2), ('runtime-error-then-timeout', False, 2)]


def main():
    current, requests = {}, []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            case = current['case']
            requests.append(case)
            index = len(requests)
            if case.startswith('runtime-') and case not in ('runtime-substream', 'runtime-malformed', 'runtime-http-error'):
                if not case.endswith('-then-timeout') or index > 1:
                    time.sleep(.2)
            if case == 'timeout' or (case == 'transient' and index == 1):
                time.sleep(3)
            if case in ('owner-change', 'stopping', 'setup-ap'):
                time.sleep(.6)
            cfg = {'audio': {'enabled': True, 'srate': '48000'}, 'records': {'enabled': False},
                   'video0': {'enabled': True, 'size': '2560x1440'},
                   'video1': {'enabled': case in ('substream', 'runtime-substream') or (case == 'runtime-invalid-then-timeout' and index == 1)},
                   'motionDetect': {'roi': ['0x0x0x10'] if case == 'roi' else []}}
            body = b'{' if case in ('malformed', 'runtime-malformed') or (case == 'runtime-malformed-then-timeout' and index == 1) else json.dumps(cfg).encode()
            self.send_response(503 if case in ('http-error', 'runtime-http-error') or (case == 'runtime-error-then-timeout' and index == 1) else 200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = 'http://127.0.0.1:' + str(server.server_port) + '/api/v1/config.json'
    with tempfile.TemporaryDirectory(prefix='c120-model-ready-') as folder:
        root = Path(folder)
        deps = root / 'sound-deps'
        subprocess.run(['sh', str(BASE / 'sound-deps.sh'), str(deps)], check=True)
        harness = root / 'ready.c'
        harness.write_text('''#define _GNU_SOURCE
#define access test_access
#define main aid_main
#include "''' + str(BASE / 'c120-aid.c') + '''"
#undef main
#undef access
#include <sys/prctl.h>
static volatile sig_atomic_t test_ap,change_mode;
int test_access(const char *path,int mode) {
    if (!strcmp(path,AP)) return test_ap?0:-1;
    return faccessat(AT_FDCWD,path,mode,0);
}
static void change(int sig) {
    (void)sig;
    if (change_mode==1) ++owner_pid;
    if (change_mode==2) stopping=1;
    if (change_mode==3) test_ap=1;
}
int main(int argc,char **argv) {
    assert(argc==2 && !prctl(PR_SET_NAME,"majestic",0,0,0));
    settings=defaults(); add(settings,"enabled",json_object_new_boolean(1));
    regions=json_object_new_array(); owner_pid=camera_pid(); assert(owner_pid==getpid());
    assert(!curl_global_init(CURL_GLOBAL_DEFAULT));
    if (!strcmp(argv[1],"owner-before")) ++owner_pid;
    if (!strcmp(argv[1],"owner-change")) change_mode=1;
    if (!strcmp(argv[1],"stopping")) change_mode=2;
    if (!strcmp(argv[1],"setup-ap")) change_mode=3;
    if (change_mode) { signal(SIGALRM,change); ualarm(300000,0); }
    int runtime=!strncmp(argv[1],"runtime-",8);
    int ready;
    if (runtime) {
        device_ready=1; pipeline_verified=now();
        if (!strcmp(argv[1],"runtime-expired")) pipeline_verified-=11;
        if (!strcmp(argv[1],"runtime-owner")) ++owner_pid;
        if (!strcmp(argv[1],"runtime-stopping")) stopping=1;
        if (!strcmp(argv[1],"runtime-ap")) test_ap=1;
        if (!strcmp(argv[1],"runtime-no-device")) device_ready=0;
        if (!strcmp(argv[1],"runtime-cleanup")) engine_stopping=1;
        if (!strcmp(argv[1],"runtime-never-verified")) pipeline_verified=0;
        ready=runtime_pipeline_ready();
        if (strstr(argv[1],"-then-timeout")) {
            assert(!ready && !pipeline_verified);
            ready=runtime_pipeline_ready();
        }
    } else ready=model_pipeline_ready();
    ualarm(0,0);
    printf("%d %s\\n",ready,runtime?reason:model_error);
    json_object_put(settings); json_object_put(regions); curl_global_cleanup();
    return 0;
}
''')
        libs = subprocess.check_output(['pkg-config', '--cflags', '--libs',
                                        'json-c', 'libcurl', 'ogg', 'opus', 'zlib'], text=True).split()
        executable = root / 'ready'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                        '-DC120_HOST_TEST', '-DC120_CONFIG_URL="' + url + '"',
                        '-I' + str(deps / 'kissfft'), str(harness),
                        *[str(BASE / name) for name in ['sound.c', 'opus-input.c', 'notify.c', 'record.c',
                                                       'catalogue.c', 'models.c', 'bird.c']],
                        str(deps / 'kissfft/kiss_fft.c'),
                        '-o', str(executable), *libs, '-lm', '-ldl'], check=True)
        for case, expected, count in CASES+RUNTIME:
            current['case'] = case
            requests.clear()
            started = time.monotonic()
            output = subprocess.check_output([str(executable), case], text=True, timeout=12).strip()
            elapsed = time.monotonic() - started
            assert output.split()[0] == str(int(expected)), (case, output)
            assert len(requests) == count, (case, requests)
            if case.startswith('runtime-'):
                assert elapsed < .4, (case, elapsed)
            if case == 'timeout':
                assert 'Waiting for camera' in output and elapsed < 9, (output, elapsed)
            if case in ('owner-change', 'owner-before', 'stopping', 'setup-ap'):
                assert 'Camera stopped or setup AP active' in output, output
            if case == 'substream':
                assert 'substream uses analysis resources' in output, output
            if case == 'roi':
                assert 'invalid motion regions' in output, output
            print('PASS:', case, 'requests=' + str(count), 'seconds=' + str(round(elapsed, 3)))
    server.shutdown()
    server.server_close()


if __name__ == '__main__':
    main()

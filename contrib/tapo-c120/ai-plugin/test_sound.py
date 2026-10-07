"""Host AI validation, Opus and DSP regressions; no models or camera samples required."""
from pathlib import Path
import json
import re
import subprocess
import tempfile
import numpy as np

ROOT = Path(__file__).resolve().parent


def api_status(out, libs):
    source = (ROOT/'c120-aid.c').read_text()
    for key in ('CONFIG','STATE','PIDFILE','TOKEN','AP','RECORD_REQUEST'):
        source, count = re.subn(r'^#define '+key+r' "[^"]+"',
                               '#define '+key+' "'+str(out/key)+'"',source,flags=re.M)
        assert count == 1
    (out/'mounts').write_text('')
    harness = out/'api.c'
    harness.write_text('#define main daemon_main\n'+source+r'''
#undef main
#include <sys/inotify.h>
#include <sys/prctl.h>
int main(int argc,char **argv) {
    assert(argc==2); const char *mode=argv[1];
    int full=!strcmp(mode,"full"),post=!strcmp(mode,"post");
    assert(!prctl(PR_SET_NAME,"c120-aid",0,0,0));
    FILE *pid=fopen(PIDFILE,"w"); assert(pid); fprintf(pid,"%ld\n",(long)getpid()); fclose(pid);
    J *o=defaults(); assert(!atomic_json(CONFIG,o,0)); json_object_put(o);
    o=json_tokener_parse("{\"running\":true,\"soundRunning\":true,\"status\":\"Detecting\",\"objects\":[{}],\"sounds\":[{}]}");
    add(o,"monotonic",json_object_new_double(now()-(!strcmp(mode,"stale")?20:0)));
    assert(!atomic_json(STATE,o,0)); json_object_put(o);
    unlink(AP); if (!strcmp(mode,"ap")) { int fd=open(AP,O_CREAT|O_WRONLY,0600); assert(fd>=0); close(fd); }
    if (!strcmp(mode,"stopped")) unlink(PIDFILE);
    setenv("REQUEST_METHOD",post?"POST":"GET",1); setenv("QUERY_STRING",full?"":"view=status",1);
    if (post) { setenv("CONTENT_TYPE","application/json",1); setenv("CONTENT_LENGTH","14",1); }
    int fd=inotify_init1(IN_NONBLOCK); assert(fd>=0);
    int config=inotify_add_watch(fd,CONFIG,IN_OPEN), mounts=inotify_add_watch(fd,C120_MODEL_MOUNTS,IN_OPEN);
    assert(config>=0 && mounts>=0);
    int result=api(); char events[4096]; ssize_t n=read(fd,events,sizeof(events));
    int read_config=0,read_models=0;
    for (ssize_t i=0;i<n;) {
        struct inotify_event e; memcpy(&e,events+i,sizeof(e));
        if (e.wd==config) read_config=1;
        if (e.wd==mounts) read_models=1;
        i+=sizeof(e)+e.len;
    }
    assert(read_config==full && read_models==full); close(fd); return result;
}
''')
    exe = out/'api-test'
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-Wno-sign-compare',
                    '-DC120_HOST_TEST',f'-DC120_MODEL_MOUNTS="{out}/mounts"',
                    '-I'+str(ROOT),'-I'+str(out/'kissfft'),str(harness),
                    *[str(ROOT/name) for name in ('sound.c','opus-input.c','notify.c','record.c','catalogue.c','models.c','bird.c')],
                    str(out/'kissfft/kiss_fft.c'),'-o',str(exe),*libs,'-lm','-ldl'],check=True)
    for mode in ('full','light','stale','stopped','ap','post'):
        raw = subprocess.check_output([str(exe),mode],input=b'{"csrf":"bad"}')
        header, body = raw.decode().split('\n\n',1)
        data = json.loads(body)
        assert f" {503 if mode=='ap' else 403 if mode=='post' else 200} " in header
        if mode in ('ap','post'): continue
        assert ('config' in data) == ('models' in data) == (mode=='full')
        assert len(data['csrf'])==64 and data['schemaVersion']==2
        if mode in ('stale','stopped'):
            assert not data['running'] and not data['soundRunning'] and not data['objects'] and not data['sounds']
        else:
            assert data['running'] and data['soundRunning']
    print('PASS: light API skips settings/model I/O, preserves full API, CSRF/AP/stale/stopped guards')


def reference(pcm):
    frames = np.lib.stride_tricks.sliding_window_view(pcm, 512)[::160][:101]
    window = (.5-.5*np.cos(2*np.pi*np.arange(512)/512)).astype(np.float32)
    fft = np.fft.rfft(frames*window)
    power = (fft.real**2+fft.imag**2).astype(np.float32)
    power[:, 0] = np.abs(fft[:, 0]); power[:, -1] = np.abs(fft[:, -1])
    points = (700*(np.power(1+4000/700,np.arange(66)/65)-1)).astype(np.float32)
    hz = np.arange(257,dtype=np.float32)*(8000/512)
    bank = np.maximum(0,np.minimum((hz-points[:-2,None])/(points[1:-1]-points[:-2])[:,None],
                                  (points[2:,None]-hz)/(points[2:]-points[1:-1])[:,None]))
    bank[bank < .0001] = 0
    log = 10*np.log10(np.maximum(np.abs(power@bank.T),1e-10))
    log = np.maximum(log,log.max(axis=1,keepdims=True)-80)
    return np.clip(log.T/np.float32(.0030518509447575),-32768,32767).astype('<i2')


def main():
    with tempfile.TemporaryDirectory(prefix='c120-sound-test-') as folder:
        out=Path(folder)
        subprocess.run(['sh',str(ROOT/'sound-deps.sh'),folder],check=True)
        libs = subprocess.check_output(['pkg-config', '--cflags', '--libs',
                                        'json-c', 'libcurl', 'ogg', 'opus', 'zlib'], text=True).split()
        aid = out/'c120-aid'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                        '-DC120_HOST_TEST', '-I'+str(out/'kissfft'),
                        *[str(ROOT/name) for name in ('c120-aid.c', 'sound.c', 'opus-input.c', 'notify.c', 'record.c', 'catalogue.c', 'models.c', 'bird.c')],
                        str(out/'kissfft/kiss_fft.c'),
                        '-o', str(aid), *libs, '-lm', '-ldl'], check=True)
        subprocess.run([str(aid), '--self-test'], check=True)
        api_status(out,libs)
        for args in ([], ['api'], ['--trial', '60']):
            result = subprocess.run([str(aid), *args], capture_output=True, text=True)
            assert result.returncode == 2 and 'only support --self-test' in result.stderr
        opus = out/'test-opus'
        subprocess.run(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(ROOT/'test_opus.c'), str(ROOT/'opus-input.c'),
                        '-o', str(opus), *libs, '-lm'], check=True)
        subprocess.run([str(opus)], check=True)
        exe=str(out/'sound-test')
        subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-Wno-sign-compare',
                        '-DSOUND_TEST','-I'+str(out/'kissfft'),str(ROOT/'sound.c'),
                        str(out/'kissfft/kiss_fft.c'),'-lm','-o',exe],check=True)
        rng=np.random.default_rng(1234)
        for pcm in (np.zeros(40000,dtype='<i2'), rng.integers(-12000,12000,40000,dtype=np.int16),
                    (10000*np.sin(2*np.pi*440*np.arange(40000)/8000)).astype('<i2')):
            native=np.frombuffer(subprocess.check_output([exe,'8000'],input=pcm.tobytes()),dtype='<i2').reshape(-1,64,101)
            expected=np.array([reference(pcm[n:n+16512].astype(np.float32)/32768)
                               for n in range(0,len(pcm)-16512+1,1920)])
            assert native.shape==expected.shape
            assert np.max(np.abs(native.astype(int)-expected.astype(int)))<=2
        # Native Opus decoding provides 16 kHz; stock analysis keeps every other sample.
        for rate in (8000,16000):
            pcm=np.zeros(rate*5,dtype='<i2')
            raw=subprocess.check_output([exe,str(rate)],input=pcm.tobytes())
            assert len(raw)==13*64*101*2,(rate,len(raw))
            assert np.all(np.frombuffer(raw,dtype='<i2')==-32767)
        for rate in (0,7999,32000,44100,48000):
            assert subprocess.run([exe,str(rate)],input=b'').returncode != 0
        pcm=rng.integers(-12000,12000,80000,dtype=np.int16)
        assert subprocess.check_output([exe,'16000'],input=pcm.tobytes()) == subprocess.check_output(
            [exe,'8000'],input=pcm[::2].tobytes())
        pcm=rng.integers(-20000,20000,16512,dtype=np.int16)
        native=np.frombuffer(subprocess.check_output([exe,'8000','12'],input=pcm.tobytes()),dtype='<i2').reshape(64,101)
        expected=reference(np.clip(pcm.astype(np.float32)/32768*np.float32(10**(.6)),-1,1))
        assert np.max(np.abs(native.astype(int)-expected.astype(int)))<=2
        assert subprocess.run([exe,'8000','25'],input=b'').returncode!=0
    print('PASS: sparse FFT/mel/quantization, rolling windows, silence and exact 16-to-8 kHz decimation')


if __name__=='__main__':
    main()

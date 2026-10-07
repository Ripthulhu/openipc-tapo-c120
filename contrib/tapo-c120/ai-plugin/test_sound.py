"""Host AI validation, Opus and DSP regressions; no models or camera samples required."""
from pathlib import Path
import subprocess
import tempfile
import numpy as np

ROOT = Path(__file__).resolve().parent


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
                        '-DC120_HOST_TEST', '-DOUTSIDE_SPEEX', '-DRANDOM_PREFIX=c120', '-DFLOATING_POINT',
                        '-I'+str(out/'kissfft'), '-I'+str(out/'speex'),
                        *[str(ROOT/name) for name in ('c120-aid.c', 'sound.c', 'opus-input.c', 'notify.c', 'record.c', 'models.c', 'bird.c')],
                        str(out/'kissfft/kiss_fft.c'), str(out/'speex/resample.c'),
                        '-o', str(aid), *libs, '-lm', '-ldl'], check=True)
        subprocess.run([str(aid), '--self-test'], check=True)
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
                        '-DSOUND_TEST','-DOUTSIDE_SPEEX','-DRANDOM_PREFIX=c120','-DFLOATING_POINT',
                        '-I'+str(out/'kissfft'),'-I'+str(out/'speex'),str(ROOT/'sound.c'),
                        str(out/'kissfft/kiss_fft.c'),str(out/'speex/resample.c'),'-lm','-o',exe],check=True)
        rng=np.random.default_rng(1234)
        for pcm in (np.zeros(40000,dtype='<i2'), rng.integers(-12000,12000,40000,dtype=np.int16),
                    (10000*np.sin(2*np.pi*440*np.arange(40000)/8000)).astype('<i2')):
            native=np.frombuffer(subprocess.check_output([exe,'8000'],input=pcm.tobytes()),dtype='<i2').reshape(-1,64,101)
            expected=np.array([reference(pcm[n:n+16512].astype(np.float32)/32768)
                               for n in range(0,len(pcm)-16512+1,1920)])
            assert native.shape==expected.shape
            assert np.max(np.abs(native.astype(int)-expected.astype(int)))<=2
        # Resampling does not change the analysis cadence, including non-integer ratios.
        for rate in (8000,16000,32000,44100,48000):
            pcm=np.zeros(rate*5,dtype='<i2')
            raw=subprocess.check_output([exe,str(rate)],input=pcm.tobytes())
            assert len(raw)==13*64*101*2,(rate,len(raw))
            assert np.all(np.frombuffer(raw,dtype='<i2')==-32767)
        pcm=rng.integers(-20000,20000,16512,dtype=np.int16)
        native=np.frombuffer(subprocess.check_output([exe,'8000','12'],input=pcm.tobytes()),dtype='<i2').reshape(64,101)
        expected=reference(np.clip(pcm.astype(np.float32)/32768*np.float32(10**(.6)),-1,1))
        assert np.max(np.abs(native.astype(int)-expected.astype(int)))<=2
        assert subprocess.run([exe,'8000','25'],input=b'').returncode!=0
    print('PASS: native FFT/mel/quantization, rolling windows, silence and five sample rates')


if __name__=='__main__':
    main()

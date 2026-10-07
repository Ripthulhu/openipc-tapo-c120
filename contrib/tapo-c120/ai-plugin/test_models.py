"""Host model-store and raw-head checks; --golden accepts private offline photo evidence."""
import argparse
import base64
import ctypes as C
from ctypes.util import find_library
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import numpy as np

BASE = Path(__file__).resolve().parent


def test_fat_install():
    with tempfile.TemporaryDirectory(prefix='c120-fat-install-') as folder:
        root = Path(folder)
        source, target = root/'source', root/'target'
        executable = ['usr/bin/c120-ai', 'usr/bin/c120-recording-closed', 'usr/lib/c120-ai/c120-aid', 'etc/init.d/S97c120-ai',
                      'var/www/cgi-bin/c120-ai.cgi', 'var/www/cgi-bin/c120-ai-api.cgi', 'var/www/cgi-bin/c120-recordings-api.cgi']
        readonly = ['usr/lib/c120-ai/libmi_ipu.so', 'usr/lib/c120-ai/objects.img.gz',
                    'usr/lib/c120-ai/LICENSE-test', 'var/www/a/c120-ai.js']
        for name in executable + readonly + ['etc/c120-ai.json']:
            path = source/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture\n')
            path.chmod(0o777)
        parents = ['usr', 'usr/bin', 'usr/lib', 'var', 'var/www', 'var/www/a',
                   'var/www/cgi-bin', 'etc', 'etc/init.d']
        for name in parents:
            (target/name).mkdir(parents=True, exist_ok=True)
            (target/name).chmod(0o750)
        installer = (BASE/'install.sh').read_text()
        block = installer.split('mkdir -p /usr/lib/c120-ai /var/www/a\n', 1)[1].split('ap=/usr/bin/', 1)[0]
        block = 'mkdir -p /usr/lib/c120-ai /var/www/a\n' + block
        block = re.sub(r'(?<!\$src)/(usr|var|etc)/', lambda m: str(target)+'/'+m[1]+'/', block)
        subprocess.run(['sh', '-eu', '-s', str(source)], input='umask 077\nsrc=$1\n'+block,
                       text=True, check=True)
        for name in parents: assert (target/name).stat().st_mode & 0o777 == 0o750, name
        for name in executable: assert (target/name).stat().st_mode & 0o777 == 0o755, name
        for name in readonly: assert (target/name).stat().st_mode & 0o777 == 0o644, name
        assert (target/'etc/c120-ai.json').stat().st_mode & 0o777 == 0o600
        assert os.readlink(target/'usr/lib/c120-ai/libc.so.0') == '/lib/libc.so'
    print('PASS: FAT install modes, on-flash compatibility link, existing system directories preserved')


class Tensor(C.Structure):
    _fields_ = [('width', C.c_uint), ('height', C.c_uint), ('channels', C.c_uint),
                ('stride', C.c_uint), ('bytes', C.c_uint), ('scale', C.c_float), ('zero', C.c_int64)]


class Box(C.Structure):
    _fields_ = [(s, C.c_float) for s in ('x1', 'y1', 'x2', 'y2', 'score')] + [('index', C.c_uint)]


class Image(C.Structure):
    _fields_ = [(s, C.c_void_p) for s in ('rgb', 'y', 'uv')] + [(s, C.c_uint) for s in
                ('width', 'height', 'rgb_stride', 'y_stride', 'uv_stride')]


class Model(C.Structure):
    _fields_ = [('id', C.c_char * 49), ('name', C.c_char * 81), ('sha256', C.c_char * 65),
                ('bytes', C.c_uint), ('bird', C.c_int), ('fd', C.c_int), ('confidence', C.c_double), ('nms', C.c_double)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--golden', type=Path)
    args = parser.parse_args()
    test_fat_install()
    with tempfile.TemporaryDirectory(prefix='c120-model-test-') as folder, tempfile.TemporaryDirectory(prefix='c120-card-test-', dir='/dev/shm') as card_folder:
        root = Path(folder)
        mounts = root/'mounts'
        card = Path(card_folder)
        mounts.write_text(f'/dev/mmcblk0p1 {card} ext4 rw 0 0\n')
        shim = root/'space.c'
        shim.write_text('#undef fstatvfs\n#include <sys/statvfs.h>\nstatic int full;\nvoid test_full(int n){full=n;}\nint test_fstatvfs(int fd,struct statvfs *s){int r=fstatvfs(fd,s);if(!r && full)s->f_bavail=0;return r;}\n')
        flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'json-c'], text=True).split()
        so = root/'models.so'
        subprocess.run(['gcc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Dfstatvfs=test_fstatvfs', f'-DC120_MODEL_MOUNTS="{mounts}"',
                        f'-DC120_MODEL_LOCK="{root}/lock"', '-DC120_MODEL_PREFIX="/dev/shm/"',
                        str(BASE/'models.c'), str(BASE/'bird.c'), str(shim), '-o', str(so), *flags, '-lm'], check=True)
        lib = C.CDLL(str(so))
        js = C.CDLL(find_library('json-c'))
        js.json_tokener_parse.argtypes = [C.c_char_p]
        js.json_tokener_parse.restype = C.c_void_p
        js.json_object_put.argtypes = [C.c_void_p]
        js.json_object_to_json_string_ext.argtypes = [C.c_void_p, C.c_int]
        js.json_object_to_json_string_ext.restype = C.c_char_p
        lib.models_state.argtypes = [C.c_char_p, C.c_char_p]
        lib.models_state.restype = C.c_void_p
        lib.models_request.argtypes = [C.c_void_p, C.c_char_p, C.c_char_p, C.c_char_p]
        lib.models_request.restype = C.c_void_p
        lib.model_open.argtypes = [C.c_char_p, C.POINTER(Model), C.c_char_p]

        def result(o):
            assert o
            try:
                return json.loads(js.json_object_to_json_string_ext(o, 0))
            finally:
                js.json_object_put(o)

        def request(action, id='bird-test', protected=(b'stock', b'stock', b'stock'), **data):
            o = js.json_tokener_parse(json.dumps(dict(modelAction=action, id=id, **data)).encode())
            try:
                return result(lib.models_request(o, *protected))
            finally:
                js.json_object_put(o)

        def opened(id, valid=True):
            m, err = Model(), C.create_string_buffer(160)
            rc = lib.model_open(id.encode(), C.byref(m), err)
            assert (rc == 0) == valid, (id, rc, err.value)
            if rc == 0 and m.fd >= 0:
                os.close(m.fd)
            return m

        payload = bytes(range(256))*16
        profile = json.loads((BASE/'bird-coco-profile.json').read_text())
        profile.update(id='bird-test', modelBytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
        assert not request('start', profile=profile).get('error')
        assert result(lib.models_state(b'stock', b'stock'))['uploads'] == ['bird-test']
        opened('bird-test', False)
        assert request('finish').get('error')
        assert request('chunk', offset=1, data=base64.b64encode(payload[:3072]).decode()).get('error')
        assert request('chunk', offset=2**63-1, data=base64.b64encode(payload[:3072]).decode()).get('error')
        assert request('chunk', offset=0, data='YR==').get('error'), 'noncanonical base64 accepted'
        for offset in range(0, len(payload), 3072):
            assert not request('chunk', offset=offset, data=base64.b64encode(payload[offset:offset+3072]).decode()).get('error')
        assert not request('finish').get('error')
        assert opened('bird-test').bird == 1
        assert len(result(lib.models_state(b'bird-test', b'bird-test'))['items']) == 2
        for protected in [(b'bird-test', b'stock', b'stock'), (b'stock', b'bird-test', b'stock'), (b'stock', b'stock', b'bird-test')]:
            assert request('remove', protected=protected).get('error')
        model = card/'c120-ai/models/bird-test/model.img'
        model.write_bytes(payload[:-1] + b'\0')
        opened('bird-test', False)
        assert not request('remove').get('error')
        for change in [dict(id='../etc'), dict(id='bad\0suffix'), dict(decoder='ssd'), dict(target='ssc335'),
                       dict(sha256='f'*63), dict(modelBytes=9*1024*1024), dict(confidence=.1), dict(nms=1), dict(extra=1)]:
            assert request('start', id=change.get('id', 'bird-test'), profile={**profile, **change}).get('error')
        assert request('start', id='stock', profile=profile).get('error')
        lib.test_full(1)
        assert request('start', profile=profile).get('error')
        lib.test_full(0)
        assert not request('start', profile=profile).get('error')
        assert not request('cancel').get('error')
        # A card's directory symlinks must not escape to another storage location.
        os.symlink(root, card/'c120-ai/models/bird-test')
        opened('bird-test', False)
        assert request('remove').get('error')
        (card/'c120-ai/models/bird-test').unlink()
        for options in ['rw,relatime,errors=remount-ro', 'rw,errors=remount-ro,nosuid']:
            mounts.write_text(f'/dev/mmcblk0 {card} vfat {options} 0 0\n')
            assert result(lib.models_state(b'stock', b'stock'))['writable']
        for options in ['ro', 'ro,relatime', 'relatime,ro', 'relatime,ro,nosuid']:
            mounts.write_text(f'/dev/mmcblk0p1 {card} ext4 {options} 0 0\n')
            assert not result(lib.models_state(b'stock', b'stock'))['writable']
            assert request('start', profile=profile).get('error')
        mounts.write_text('')
        opened('bird-test', False)
        assert result(lib.models_state(b'stock', b'stock'))['storage'] == 'No mounted SD card'
        mounts.unlink()
        assert result(lib.models_state(b'stock', b'stock'))['storage'] == 'No mounted SD card'
        mounts.write_text(f'/dev/mmcblk0p1 {card} ext4 rw 0 0\n')
        assert not request('start', profile=profile).get('error'), 'late card was not rediscovered'
        request('cancel')
        print('PASS: bounded upload, atomic completion, hashes, safe paths, incomplete/corrupt/unsupported models, deletion guards, absent/late/read-only/full SD')

        lib.bird_tensor_desc.argtypes = [C.c_void_p, C.c_uint, C.c_uint, C.c_uint, C.POINTER(Tensor)]
        lib.bird_quantize.argtypes = [C.c_void_p, C.c_uint, C.POINTER(Tensor), C.c_void_p]
        lib.bird_prepare.argtypes = [C.POINTER(Image), C.POINTER(Tensor), C.c_void_p]
        lib.bird_decode.argtypes = [C.POINTER(Tensor), C.POINTER(C.c_void_p), C.c_float, C.c_float, C.c_uint, C.c_uint, C.POINTER(Box)]
        desc = np.zeros(90, dtype='<u4')
        desc[:6] = [4, 2, 1, 320, 320, 3]
        desc[76], desc[80] = 6, 614400
        desc[77] = np.array([3.0518509447574615e-05], dtype='<f4').view('<u4')[0]
        t = Tensor()
        assert lib.bird_tensor_desc(desc.ctypes.data, 320, 320, 3, C.byref(t)) == 0
        desc[76] = 7
        assert lib.bird_tensor_desc(desc.ctypes.data, 320, 320, 3, C.byref(t)) == -1
        desc[76] = 6
        assert lib.bird_tensor_desc(desc.ctypes.data, 320, 320, 3, C.byref(t)) == 0
        rng = np.random.default_rng(14)
        rgb = rng.integers(0, 256, (320, 320, 3), dtype=np.uint8)
        output = np.zeros((320, 320, 3), dtype='<i2')
        im = Image(rgb.ctypes.data, None, None, 320, 320, 960, 0, 0)
        assert lib.bird_prepare(C.byref(im), C.byref(t), output.ctypes.data) == 0
        expected = np.clip((rgb.astype(np.float32)/np.float32(255))/np.float32(t.scale), -32768, 32767).astype('<i2')
        assert np.array_equal(output, expected)
        y = np.full((450, 800), 16, dtype=np.uint8)
        uv = np.full((225, 800), 128, dtype=np.uint8)
        im = Image(None, y.ctypes.data, uv.ctypes.data, 800, 450, 0, 800, 800)
        assert lib.bird_prepare(C.byref(im), C.byref(t), output.ctypes.data) == 0
        assert np.all(output[70:250] == 0) and np.all(output[:70] == int(np.float32(114/255)/np.float32(t.scale)))
        heads = (Tensor*3)(*[Tensor(s, s, 255, 512, s*s*512, .001, 5) for s in (40, 20, 10)])
        raw = [np.full((s, s, 256), -12000, dtype='<i2') for s in (40, 20, 10)]
        ptrs = (C.c_void_p*3)(*[p.ctypes.data for p in raw])
        boxes = (Box*300)()
        # A bird runner-up must not bypass single-label classification.
        p = raw[0][20, 20]
        p[:4], p[4], p[19], p[5] = 5, 5005, 4005, 6005
        assert lib.bird_decode(heads, ptrs, .25, .45, 800, 450, boxes) == 0
        p[5] = -12000
        assert lib.bird_decode(heads, ptrs, .25, .45, 800, 450, boxes) == 1 and boxes[0].index == 820
        p[4], p[19] = 5, 5  # sigmoid(0)*sigmoid(0) == .25: strict cutoff.
        assert lib.bird_decode(heads, ptrs, .25, .45, 800, 450, boxes) == 0
        assert lib.bird_decode(heads, ptrs, .25, 1.0, 800, 450, boxes) == -1
        print('PASS: tensor pitches/quantization, RGB/NV12 letterbox, padding, best-class policy, strict cutoff and thresholds')

        if args.golden:
            from PIL import Image as PillowImage
            for width, height in [(800, 450), (800, 448), (640, 360), (321, 180), (320, 320)]:
                pixels = rng.integers(0, 256, (height, width, 3), dtype=np.uint8)
                im = Image(pixels.ctypes.data, None, None, width, height, width*3, 0, 0)
                assert lib.bird_prepare(C.byref(im), C.byref(t), output.ctypes.data) == 0
                gain = min(320/width, 320/height)
                size = (round(width*gain), round(height*gain))
                canvas = PillowImage.new('RGB', (320, 320), (114, 114, 114))
                canvas.paste(PillowImage.fromarray(pixels).resize(size, PillowImage.Resampling.BILINEAR),
                             (round((320-size[0])/2-.1), round((320-size[1])/2-.1)))
                expected = np.clip((np.asarray(canvas, dtype=np.float32)/np.float32(255))/np.float32(t.scale), -32768, 32767).astype('<i2')
                assert np.array_equal(output, expected), (width, height, int(np.max(np.abs(output.astype(int)-expected.astype(int)))))
            print('PASS: RGB bilinear/letterbox byte-identical to Pillow on five synthetic camera-size/asymmetric inputs')
            evidence = json.loads((args.golden/'results.json').read_text())
            metadata = evidence['native_metadata']
            checked = 0
            for photo in evidence['photos']:
                data = np.load(args.golden/photo['input_array'])
                native = np.load(args.golden/photo['encoded_input_array'])
                tin = Tensor(320, 320, 3, native.shape[-1]*2, native.nbytes, *metadata['input']['quantization'])
                encoded = np.empty_like(native)
                assert lib.bird_quantize(data.ctypes.data, 320*320, C.byref(tin), encoded.ctypes.data) == 0
                assert np.array_equal(encoded, native), photo['prefix']
                arrays = [np.load(args.golden/head['raw_array']) for head in photo['heads']]
                tensors = (Tensor*3)(*[Tensor(a.shape[2], a.shape[1], 255, a.shape[3]*2, a.nbytes,
                                              head['quantization']['scale'], int(head['quantization']['zero_point']))
                                       for a, head in zip(arrays, photo['heads'])])
                pointers = (C.c_void_p*3)(*[a.ctypes.data for a in arrays])
                n = lib.bird_decode(tensors, pointers, .25, .45, photo['width'], photo['height'], boxes)
                gold = photo['detections']
                assert n == len(gold), (photo['prefix'], n, len(gold))
                for actual, reference in zip(boxes[:n], gold):
                    assert actual.index == reference['candidate_index']
                    assert abs(actual.score-reference['score']) < 2e-6
                    expected = np.array(reference['box_xyxy'])/[photo['width'], photo['height'], photo['width'], photo['height']]
                    assert np.max(np.abs(np.array([actual.x1, actual.y1, actual.x2, actual.y2])-expected)) < 2e-6
                checked += 1
            print('PASS: byte-identical saved quantized inputs and raw-head detections on', checked, 'offline photos')


if __name__ == '__main__':
    main()

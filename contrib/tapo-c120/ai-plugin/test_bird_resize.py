"""Exact pre-optimization resize fixtures, including row-cache wrap and padded input."""
import ctypes as C
import hashlib
from pathlib import Path
import subprocess
import tempfile
import numpy as np
from test_models import Image, Tensor

BASE = Path(__file__).resolve().parent
# Generated from the full-frame implementation in commit 101eca1b.
GOLDEN = {
    '320-rgb-800x450': '5de9ea1b4056d562fb0af0f33d3ae029c93af741c2acb154ced2e9a5f4eebe66',
    '320-rgb-450x800': 'ad57be9ed65c2af35b85c7d4f5511d6c48aa8df301d941cd12c88e9c99d7149b',
    '320-rgb-1600x900': '5673f811872f3d9245f3541ef89655c3dee3f944ef0c57b2ccfa928524b5c63e',
    '320-rgb-321x181': '0215c8a44f5d3f5e4a4fe8afb4d4a7a0935d82b1707ded456ad8c1300e54b2e8',
    '320-rgb-160x90': '2b864c050792f7321ce8eea83e7a6179ad3d6c3a939e97236a47f6beec0398dc',
    '320-rgb-2x2': '70457deea1365997ff8ede26d4feb41a8f6e036f174803bad7ca3723a5d393da',
    '320-nv12-800x450': '45502ba4da438fd37a186b38b015d26f15ea15f66f782ac23d1a1b1125f7601e',
    '320-nv12-1600x900': '95d0f3250c694c94cef7cab9f42c8abdca9264bca3128cff2ca64222a9adb8a0',
    '384-rgb-800x450': 'be160508c2a67b5c27167be9cd1eafbbfecdb5792221d56c73969ac9a000476a',
    '384-rgb-450x800': 'e970a806073afc1c45a9d42566857eaf9e3bbb503213697d55a894ceb6f5f246',
    '384-rgb-1600x900': 'c3f0e05f036f2e437ef4e21a1022ef51683b4667eba933e34b788b3f27d367e0',
    '384-rgb-321x181': 'cae6fc72b7cb415b8ea37b900aefec68b1bc47b3a626af6355d5bc2f4253583d',
    '384-rgb-160x90': '2652fcc0e49d96933ae615e401b675458dca9d5c5e737e3c6ffd4b525b054df8',
    '384-rgb-2x2': 'e98c22594b17042443714f41b772b96532d6fa36827392157e438cde676b3d0c',
    '384-nv12-800x450': 'cd94d014201a223921071412cd553136f4167c5ed4ee4bb1961d1b992396c62f',
    '384-nv12-1600x900': '6d888e6d51eb336b45e30c65ff8cf1b887009eef62f58efc74105706135a63db',
}


def outputs(lib):
    lib.bird_prepare.argtypes = [C.POINTER(Image), C.POINTER(Tensor), C.c_void_p]
    hashes = {}
    for side in (320, 384):
        t = Tensor(side, side, 3, 8, side*side*8, 3.0518509447574615e-05, 0)
        dest = np.empty((side, side, 4), dtype='<i2')
        for kind, width, height in [('rgb',800,450), ('rgb',450,800), ('rgb',1600,900),
                                    ('rgb',321,181), ('rgb',160,90), ('rgb',2,2),
                                    ('nv12',800,450), ('nv12',1600,900)]:
            def pixels(rows, columns):
                n = np.arange(rows*columns, dtype=np.uint32)
                n = n*np.uint32(1664525)+np.uint32(1013904223)
                return ((n ^ (n >> 11)) & 255).astype(np.uint8).reshape(rows,columns)
            rgb = pixels(height, width*3+7)
            y = pixels(height, width+8)
            uv = pixels(height//2, width+16)
            im = Image(rgb.ctypes.data if kind=='rgb' else None, y.ctypes.data, uv.ctypes.data,
                       width,height,rgb.shape[1],y.shape[1],uv.shape[1])
            assert lib.bird_prepare(C.byref(im), C.byref(t), dest.ctypes.data) == 0
            assert np.all(dest[...,3] == 0)
            hashes[f'{side}-{kind}-{width}x{height}'] = hashlib.sha256(dest.tobytes()).hexdigest()
    return hashes


def main():
    with tempfile.TemporaryDirectory() as folder:
        library = Path(folder)/'bird.so'
        subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',
                        str(BASE/'bird.c'),'-lm','-o',str(library)],check=True)
        actual = outputs(C.CDLL(str(library)))
        assert actual == GOLDEN, actual
    print('PASS: 16 byte-exact prior resize fixtures, RGB/NV12, 320/384, portrait/max/tiny inputs and row padding')


if __name__ == '__main__':
    main()

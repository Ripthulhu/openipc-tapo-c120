#!/bin/sh
# Small, pinned upstream DSP sources; only the compiled routines reach the camera.
set -eu
out=$1
fetch() {
    repo=$1 commit=$2 subdir=$3
    shift 3
    mkdir -p "$out/$subdir"
    for file in "$@"; do
        dest="$out/$subdir/$(basename "$file")"
        [ -f "$dest" ] || {
            curl -fsSL --retry 2 "https://raw.githubusercontent.com/$repo/$commit/$file" -o "$dest.tmp"
            mv "$dest.tmp" "$dest"
        }
    done
}
fetch mborgerding/kissfft 8f47a67f595a6641c566087bf5277034be64f24d kissfft \
    kiss_fft.c kiss_fft.h _kiss_fft_guts.h kiss_fft_log.h COPYING LICENSES/BSD-3-Clause
fetch xiph/speexdsp 1b28a0f61bc31162979e1f26f3981fc3637095c8 speex \
    libspeexdsp/resample.c libspeexdsp/arch.h include/speex/speex_resampler.h COPYING

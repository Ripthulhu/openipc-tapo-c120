#!/bin/sh
set -eu
[ "$#" = 4 ] || { echo "Usage: $0 FIRMWARE_BUILD IPU_LIBRARY STOCK_MODELS OUTPUT" >&2; exit 2; }
base=$(cd "$1" && pwd)
ipu=$(realpath "$2")
models=$(cd "$3" && pwd)
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$4"
out=$(cd "$4" && pwd)
sdk="$base/output/host/opt/ext-toolchain/sdk/lib"
lib="$out/usr/lib/c120-ai"
model="$models/obj_detection.sim_sgsimg.img"
check() {
    actual=$(sha256sum "$1")
    [ "${actual%% *}" = "$2" ] || { echo "Unverified input: $1" >&2; exit 1; }
}
check "$ipu" e78f2d9ad9000ce6adedef7686eb1d30625b2259bd3c86d0c2ab2a77ab1a47e3
check "$model" 07c8ffa24e9d028bf4c5bc1f5656f5a4baa958f0d5063963917e0b3f714cfb7d
check "$models/sed.sim_sgsimg8k.img" 72bdb31f95b892155b84c212c61e4774b85ccc314403b797dafea67d3850e8aa
deps="$out/../sound-deps"
sh "$src/sound-deps.sh" "$deps"
mkdir -p "$lib"
cp -a "$src/files/." "$out/"
"$base/output/host/bin/arm-openipc-linux-musleabihf-gcc" \
    -Os -Wall -Wextra -Werror -Wno-sign-compare -Wl,--export-dynamic \
    -DOUTSIDE_SPEEX -DRANDOM_PREFIX=c120 -DFLOATING_POINT -I"$deps/kissfft" -I"$deps/speex" \
    "$src/c120-aid.c" "$src/sound.c" "$src/opus-input.c" "$src/notify.c" "$src/record.c" "$src/catalogue.c" "$src/models.c" "$src/bird.c" "$deps/kissfft/kiss_fft.c" "$deps/speex/resample.c" -o "$lib/c120-aid" \
    -Wl,--whole-archive "$sdk/libuclibc-compat-static.a" -Wl,--no-whole-archive \
    -ljson-c -lcurl -logg -lopus -lz -lm -ldl
"$base/output/host/bin/arm-openipc-linux-musleabihf-strip" "$lib/c120-aid"
cp "$sdk/libuclibc-compat.so" "$lib/"
for name in libmi_common.so libcam_os_wrapper.so libcam_fs_wrapper.so libmi_sys.so libmi_scl.so; do
    cp "$base/general/package/sigmastar-osdrv-infinity6c/files/lib/$name" "$lib/"
done
cp "$ipu" "$lib/libmi_ipu.so"
# The package is staged on FAT-formatted SD cards; create this link on flash.
[ ! -L "$lib/libc.so.0" ] || rm "$lib/libc.so.0"
gzip -n -9 -c "$model" > "$lib/objects.img.gz"
gzip -n -9 -c "$models/sed.sim_sgsimg8k.img" > "$lib/sound.img.gz"
cp "$deps/kissfft/BSD-3-Clause" "$lib/LICENSE-kissfft"
cp "$deps/speex/COPYING" "$lib/LICENSE-speexdsp"
chmod 755 "$out/usr/bin/"* "$out/etc/init.d/S97c120-ai" "$out/var/www/cgi-bin/"*.cgi
(cd "$out" && find usr etc var -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
cp "$src/install.sh" "$src/uninstall.sh" "$src/README.md" "$src/bird-coco-profile.json" \
    "$src/RECORDINGS-API.md" "$src/recordings-openapi.yaml" "$src/recordings-client.py" "$out/"
chmod 755 "$out/install.sh" "$out/uninstall.sh"
tar -czf "$out/../c120-ai-plugin.tgz" -C "$out" .

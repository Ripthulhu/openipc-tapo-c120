#!/bin/sh
# Diagnostic only; deliberately not installed in a firmware overlay.
set -eu
[ "$#" = 3 ] || { echo "Usage: $0 FIRMWARE_BUILD IPU_LIBRARY OUTPUT_DIRECTORY" >&2; exit 2; }
base=$(cd "$1" && pwd)
ipu=$(realpath "$2")
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
sdk="$base/output/host/opt/ext-toolchain/sdk/lib"
expected=e78f2d9ad9000ce6adedef7686eb1d30625b2259bd3c86d0c2ab2a77ab1a47e3
actual=$(sha256sum "$ipu")
[ "${actual%% *}" = "$expected" ] || { echo "Untested IPU library; see README.md" >&2; exit 1; }
mkdir -p "$out/lib"
"$base/output/host/bin/arm-openipc-linux-musleabihf-gcc" \
    -Os -Wall -Wextra -Werror -Wl,--export-dynamic \
    "$src/ipu-probe.c" -o "$out/ipu-probe" \
    -Wl,--whole-archive "$sdk/libuclibc-compat-static.a" -Wl,--no-whole-archive -ldl
cp "$sdk/libuclibc-compat.so" "$out/lib/"
for lib in libmi_common.so libcam_os_wrapper.so libcam_fs_wrapper.so libmi_sys.so libmi_scl.so; do
    cp "$base/general/package/sigmastar-osdrv-infinity6c/files/lib/$lib" "$out/lib/"
done
cp "$ipu" "$out/lib/libmi_ipu.so"
ln -sf /lib/libc.so "$out/lib/libc.so.0"

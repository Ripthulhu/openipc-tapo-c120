#!/bin/sh
set -eu
[ "$#" = 2 ] || { echo "Usage: $0 BUILDROOT_OUTPUT NEW_PACKAGE_DIRECTORY" >&2; exit 2; }
build=$(cd "$1" && pwd)
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
target="$build/per-package/lame/target"
strip="$build/host/bin/arm-openipc-linux-musleabihf-strip"
readelf="$build/host/bin/arm-openipc-linux-musleabihf-readelf"
patchelf="$build/host/bin/patchelf"
[ -x "$patchelf" ] || { echo "Buildroot host-patchelf is required" >&2; exit 1; }
[ ! -e "$2" ] || { echo "Use a new package directory" >&2; exit 1; }
for file in "$target/usr/bin/lame" "$target/usr/lib/libmp3lame.so.0.0.0"; do
    "$readelf" -h "$file" | grep -q 'Machine:.*ARM' || exit 1
done
"$readelf" -l "$target/usr/bin/lame" | grep -q '/lib/ld-musl-armhf.so.1' || exit 1
mkdir -p "$2/usr/bin" "$2/usr/lib" "$2/usr/share/licenses/lame"
out=$(cd "$2" && pwd)
cp "$target/usr/bin/lame" "$out/usr/bin/"
# Regular SONAME file also works when staging on FAT, which cannot store symlinks.
cp "$target/usr/lib/libmp3lame.so.0.0.0" "$out/usr/lib/libmp3lame.so.0"
"$strip" "$out/usr/bin/lame" "$out/usr/lib/libmp3lame.so.0"
"$patchelf" --remove-rpath "$out/usr/bin/lame"
cp "$build/build/lame-3.100/COPYING" "$out/usr/share/licenses/lame/"
chmod 755 "$out/usr/bin/lame"
chmod 644 "$out/usr/lib/libmp3lame.so.0" "$out/usr/share/licenses/lame/COPYING"
cp "$src/install.sh" "$src/README.md" "$out/"
chmod 755 "$out/install.sh"
(cd "$out" && sha256sum usr/bin/lame usr/lib/libmp3lame.so.0 usr/share/licenses/lame/COPYING > SHA256SUMS)
tar -czf "$out/../openipc-mp3-tools.tgz" -C "$out" .
du -sk "$out/usr"

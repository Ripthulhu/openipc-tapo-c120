#!/bin/sh
set -eu

base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-arm-openipc-linux-musleabihf-gcc}
case "$("$CC" -dumpmachine)" in
	arm*-linux-musleabihf) ;;
	*) echo "An ARM hard-float musl compiler is required" >&2; exit 1 ;;
esac

out=${1:-"$base/build"}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
stage=$(mktemp -d "$out/.packages.XXXXXX")
binary="$out/c120-eventd"
tmp="$binary.new"
trap 'rm -f "$tmp"; rm -rf "$stage"' EXIT HUP INT TERM
"$CC" -std=c99 -Os -static -s -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	-o "$tmp" "$base/c120-eventd.c"
chmod 755 "$tmp"
mv "$tmp" "$binary"
ap="$stage/openipc-c120-ap-recovery-plugin"
runtime="$stage/openipc-c120-runtime"
mkdir -p "$ap" "$runtime"
cp -R "$base/ap-recovery-plugin/." "$ap/"
cp -R "$base/runtime-overlay" "$runtime/"
mkdir -p "$ap/files/etc/init.d"
cp "$base/install-runtime.sh" "$base/dashboard-luminance.sed" "$base/dashboard-memory.html" \
	"$base/floodlight-url.html" "$base/live-audio.html" "$base/live-feed.sed" "$base/README.md" "$runtime/"
cp "$binary" "$ap/files/usr/bin/c120-eventd"
cp "$binary" "$runtime/runtime-overlay/usr/bin/c120-eventd"
cp "$base/runtime-overlay/etc/init.d/S45c120-ap-button" "$ap/files/etc/init.d/"
(
	cd "$ap"
	sha256sum files/usr/bin/c120-eventd > eventd.sha256
)
(
	cd "$runtime"
	sha256sum runtime-overlay/usr/bin/c120-eventd > eventd.sha256
)
for name in openipc-c120-ap-recovery-plugin openipc-c120-runtime; do
	tar -czf "$stage/$name.tgz" -C "$stage" "$name"
	mv "$stage/$name.tgz" "$out/$name.tgz"
done
sha256sum "$binary" "$out/openipc-c120-ap-recovery-plugin.tgz" "$out/openipc-c120-runtime.tgz"

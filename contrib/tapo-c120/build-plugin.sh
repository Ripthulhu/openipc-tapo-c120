#!/bin/sh
set -eu

base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-arm-openipc-linux-musleabihf-gcc}
case "$("$CC" -dumpmachine)" in
	arm*-linux-musleabihf) ;;
	*) echo "An ARM hard-float musl compiler is required" >&2; exit 1 ;;
esac

binary="$base/ap-recovery-plugin/files/usr/bin/c120-eventd"
tmp="$binary.new"
trap 'rm -f "$tmp"' EXIT HUP INT TERM
"$CC" -std=c99 -Os -static -s -Wall -Wextra -Werror \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	-o "$tmp" "$base/c120-eventd.c"
chmod 755 "$tmp"
mv "$tmp" "$binary"
cp "$binary" "$base/runtime-overlay/usr/bin/c120-eventd"
cp "$base/runtime-overlay/etc/init.d/S45c120-ap-button" \
	"$base/ap-recovery-plugin/files/etc/init.d/S45c120-ap-button"
(
	cd "$base/ap-recovery-plugin"
	sha256sum files/usr/bin/c120-eventd > eventd.sha256
	cat eventd.sha256
)

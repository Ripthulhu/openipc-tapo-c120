#!/bin/sh
set -eu
[ "$#" = 2 ] || { echo "Usage: $0 OPENIPC_BUILD OUTPUT_PACKAGE" >&2; exit 2; }
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
cc="$build/output/host/bin/arm-openipc-linux-musleabihf-gcc"
strip="$build/output/host/bin/arm-openipc-linux-musleabihf-strip"
work="$out/build"
mkdir -p "$work"
version=2.0.22
archive="$work/mosquitto-$version.tar.gz"
[ -f "$archive" ] || curl -fL "https://mosquitto.org/files/source/mosquitto-$version.tar.gz" -o "$archive"
echo "2f752589ef7db40260b633fbdb536e9a04b446a315138d64a7ff3c14e2de6b68  $archive" | sha256sum -c -
tar -xzf "$archive" -C "$work"
mosq="$work/mosquitto-$version"
# Static client only; no broker, CLI processes, OpenSSL, or background thread.
make -C "$mosq/lib" -j2 libmosquitto.a WITH_TLS=no WITH_THREADING=no WITH_SOCKS=no WITH_SRV=no \
    WITH_STATIC_LIBRARIES=yes CC="$cc" AR="${cc%gcc}ar" CFLAGS='-Os -ffunction-sections -fdata-sections'
cp -R "$src/files/"* "$out/"
mkdir -p "$out/usr/bin"
"$cc" -Os -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$mosq/include" "$src/openipc-mqtt.c" "$mosq/lib/libmosquitto.a" -ljson-c -lcurl -lm -o "$out/usr/bin/openipc-mqtt"
"$strip" "$out/usr/bin/openipc-mqtt"
cp "$mosq/epl-v20" "$mosq/edl-v10" "$out/"
cp "$src/install.sh" "$out/"
cp "$src/README.md" "$src/home-assistant-home-away.yaml" "$out/"
chmod 755 "$out/usr/bin/openipc-mqtt" "$out/etc/init.d/S98openipc-mqtt" "$out/install.sh"
chmod 600 "$out/etc/openipc-mqtt.json"
(cd "$out" && find etc usr -type f -exec sha256sum {} + > SHA256SUMS)
tar -czf "$out/../openipc-mqtt-plugin.tgz" -C "$out" etc usr install.sh SHA256SUMS epl-v20 edl-v10 README.md home-assistant-home-away.yaml
du -k "$out/usr/bin/openipc-mqtt" "$out/../openipc-mqtt-plugin.tgz"

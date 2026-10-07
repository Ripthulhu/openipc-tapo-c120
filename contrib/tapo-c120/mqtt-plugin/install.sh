#!/bin/sh
set -eu
umask 077
[ "$(id -u)" = 0 ] && [ "$#" = 1 ] || { echo "Usage: $0 BACKUP_DIRECTORY" >&2; exit 2; }
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backup=$(cd "$1" && pwd)
(cd "$src" && sha256sum -c SHA256SUMS >/dev/null)
free=$(df -Pk /overlay | tail -n 1 | awk '{print $4}')
required=$(du -sk "$src/usr" "$src/etc" | awk '{n+=$1} END{print n+256}')
[ "$free" -ge "$required" ] || { echo "Insufficient flash headroom" >&2; exit 1; }
set --
for path in usr/bin/openipc-mqtt etc/init.d/S98openipc-mqtt etc/openipc-mqtt.json usr/bin/c120-setup-ap; do
    [ ! -e "/$path" ] || set -- "$@" "$path"
done
if [ "$#" != 0 ]; then
    save="$backup/openipc-mqtt-$(date +%Y%m%d-%H%M%S).tar"
    [ ! -e "$save" ] || exit 1
    tar -cf "$save" -C / "$@"
    tar -tf "$save" >/dev/null
fi
[ ! -x /etc/init.d/S98openipc-mqtt ] || /etc/init.d/S98openipc-mqtt stop
cp "$src/usr/bin/openipc-mqtt" /usr/bin/
cp "$src/etc/init.d/S98openipc-mqtt" /etc/init.d/
[ -e /etc/openipc-mqtt.json ] || cp "$src/etc/openipc-mqtt.json" /etc/
chmod 755 /usr/bin/openipc-mqtt /etc/init.d/S98openipc-mqtt
chmod 600 /etc/openipc-mqtt.json
ap=/usr/bin/c120-setup-ap
if [ -f "$ap" ] && ! grep -q 'stop_service_if_running S98openipc-mqtt' "$ap"; then
    sed -i '/stop_service_if_running S97c120-ai/i\	stop_service_if_running S98openipc-mqtt openipc-mqtt /run/openipc-mqtt.pid' "$ap"
fi
/usr/bin/openipc-mqtt --discovery >/dev/null
/etc/init.d/S98openipc-mqtt start
echo "MQTT plugin installed; configure /etc/openipc-mqtt.json and restart S98openipc-mqtt."

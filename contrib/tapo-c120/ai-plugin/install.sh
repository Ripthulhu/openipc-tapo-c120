#!/bin/sh
# Run from an extracted, verified package on persistent storage, not /tmp.
set -eu
umask 077
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ "$(id -u)" = 0 ] || exit 1
[ "$#" = 1 ] || { echo "Usage: $0 BACKUP_DIRECTORY_ON_SD" >&2; exit 2; }
backup=$(cd "$1" && pwd)
# A full model in tmpfs can starve Majestic; keep package and backup off-camera flash.
for path in "$src" "$backup"; do
    fs=$(df -P "$path" | tail -n 1 | awk '{print $1}')
    case "$fs" in /dev/mmcblk*|/dev/sd*) ;; *) echo "Use a mounted SD/USB filesystem: $path" >&2; exit 1 ;; esac
done
[ -c /dev/mi_ipu ] || { echo "Infinity6C IPU device missing" >&2; exit 1; }
[ -r /usr/lib/libogg.so.0 ] && [ -r /usr/lib/libopus.so.0 ] || {
    echo "The firmware must provide libogg and libopus for sound input" >&2; exit 1;
}
[ -f /var/www/cgi-bin/p/header.cgi ] || {
    echo "Install the OpenIPC WebUI first" >&2; exit 1;
}
[ -r /var/www/cgi-bin/p/majestic.sh ] || {
    echo "The OpenIPC WebUI recording helper is missing" >&2; exit 1;
}
. /var/www/cgi-bin/p/majestic.sh
read_hook() {
    hook_present=1
    hook=$(mj_cfg records.onClose) && return 0
    rc=$?
    [ "$rc" -eq 1 ] || { echo "Cannot read recording close hook" >&2; return 1; }
    hook=; hook_present=0
}
read_hook
(cd "$src" && sha256sum -c SHA256SUMS >/dev/null)
required=$(du -sk "$src/usr" "$src/etc" "$src/var" | awk '{n+=$1} END{print n+512}')
free=$(df -Pk /overlay | tail -n 1 | awk '{print $4}')
# Conservative even for upgrades: never fill the settings partition.
[ "$free" -ge "$required" ] || { echo "Insufficient flash headroom ($free KiB free; $required needed)" >&2; exit 1; }
save="$backup/c120-ai-$(date +%Y%m%d-%H%M%S).tar"
[ ! -e "$save" ] || { echo "Backup already exists: $save" >&2; exit 1; }
set -- var/www/cgi-bin/p/header.cgi
[ ! -f /usr/bin/c120-setup-ap ] || set -- "$@" usr/bin/c120-setup-ap
for path in usr/lib/c120-ai usr/bin/c120-ai usr/bin/c120-recording-closed etc/init.d/S97c120-ai etc/c120-ai.json etc/majestic.yaml etc/c120-recording-hook.previous var/www/cgi-bin/c120-ai.cgi var/www/cgi-bin/c120-ai-api.cgi var/www/cgi-bin/c120-recordings-api.cgi var/www/a/c120-ai.js; do
    [ ! -e "/$path" ] || set -- "$@" "$path"
done
tar -cf "$save" -C / "$@"
tar -tf "$save" >/dev/null
[ ! -x /etc/init.d/S97c120-ai ] || /etc/init.d/S97c120-ai stop
mkdir -p /usr/lib/c120-ai /var/www/a
cp "$src/usr/bin/c120-ai" /usr/bin/
cp "$src/usr/bin/c120-recording-closed" /usr/bin/
cp "$src/usr/lib/c120-ai/"* /usr/lib/c120-ai/
ln -sf /lib/libc.so /usr/lib/c120-ai/libc.so.0
cp "$src/var/www/cgi-bin/"*.cgi /var/www/cgi-bin/
cp "$src/var/www/a/c120-ai.js" /var/www/a/
cp "$src/etc/init.d/S97c120-ai" /etc/init.d/
[ -e /etc/c120-ai.json ] || cp "$src/etc/c120-ai.json" /etc/
# FAT staging does not retain Unix modes; never copy its directory permissions.
chmod 755 /usr/lib/c120-ai /usr/lib/c120-ai/c120-aid /usr/bin/c120-ai \
    /etc/init.d/S97c120-ai /var/www/cgi-bin/c120-ai.cgi /var/www/cgi-bin/c120-ai-api.cgi \
    /usr/bin/c120-recording-closed /var/www/cgi-bin/c120-recordings-api.cgi
chmod 644 /usr/lib/c120-ai/*.so /usr/lib/c120-ai/*.gz /usr/lib/c120-ai/LICENSE-* /var/www/a/c120-ai.js
chmod 600 /etc/c120-ai.json
ap=/usr/bin/c120-setup-ap
if [ -f "$ap" ] && ! grep -q 'stop_service_if_running S97c120-ai' "$ap"; then
    sed -i '/stop_service_if_running S95majestic majestic/i\	stop_service_if_running S97c120-ai c120-aid /run/c120-ai.pid' "$ap"
fi
header=/var/www/cgi-bin/p/header.cgi
if ! grep -q 'href="c120-ai.cgi"' "$header"; then
    sed -i '/href="stream-urls.cgi"/i\						<li><a class="dropdown-item" href="c120-ai.cgi">AI Detection</a></li>' "$header"
fi
sed -i 's/>C120 AI Detection</>AI Detection</g' "$header"
/usr/bin/c120-ai --self-test
# Do not replace a custom integration. Reconciliation still imports its clips.
read_hook
case "$hook" in
    ''|/usr/sbin/record.sh)
        if [ ! -e /etc/c120-recording-hook.previous ]; then
            if [ "$hook_present" -eq 0 ]; then : > /etc/c120-recording-hook.previous
            else printf '%s\n' "$hook" > /etc/c120-recording-hook.previous; fi
        fi
        mj_set records.onClose /usr/bin/c120-recording-closed string || exit 1
        ;;
    /usr/bin/c120-recording-closed) ;;
    *) echo "Preserving custom records.onClose: add c120-ai recording-closed to it for native completion webhooks." ;;
esac
/etc/init.d/S97c120-ai start
echo "AI plugin installed. Backup: $save"
echo "Configure it at /cgi-bin/c120-ai.cgi"

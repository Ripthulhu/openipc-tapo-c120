#!/bin/sh
set -eu

[ "$(id -u)" = 0 ] || { echo "Run as root" >&2; exit 1; }
BASE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HEADER=/var/www/cgi-bin/p/header.cgi
[ -f "$HEADER" ] || { echo "Install the current OpenIPC Web UI first" >&2; exit 1; }
grep -q 'href="stream-urls.cgi"' "$HEADER" || { echo "Unsupported Web UI navigation" >&2; exit 1; }
[ ! -f /run/c120-setup-ap.active ] || { echo "Stop the setup AP first" >&2; exit 1; }
(cd "$BASE/ap-recovery-plugin" && sha256sum -c eventd.sha256)
cmp "$BASE/runtime-overlay/usr/bin/c120-eventd" "$BASE/ap-recovery-plugin/files/usr/bin/c120-eventd"

BACKUP=/root/c120-runtime-backups/$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$BACKUP"
chmod 700 "$BACKUP"
cp -p "$HEADER" "$BACKUP/header.cgi"
[ ! -f /etc/c120-light-pins.conf ] || cp -p /etc/c120-light-pins.conf "$BACKUP/light-pins.conf"
[ ! -f /etc/c120-motion-light.conf ] || cp -p /etc/c120-motion-light.conf "$BACKUP/motion-light.conf"
if [ -f /var/www/a/dashboard.js ]; then
	cp -p /var/www/a/dashboard.js "$BACKUP/dashboard.js"
	sed -f "$BASE/dashboard-luminance.sed" /var/www/a/dashboard.js > /var/www/a/dashboard.js.new
	chmod 644 /var/www/a/dashboard.js.new
	mv /var/www/a/dashboard.js.new /var/www/a/dashboard.js
fi
if [ -x /etc/init.d/S45c120-ap-button ]; then
	/etc/init.d/S45c120-ap-button stop
fi
for file in "$BASE"/runtime-overlay/usr/bin/*; do
	cp "$file" /usr/bin/
	chmod 755 "/usr/bin/${file##*/}"
done
for file in "$BASE"/runtime-overlay/var/www/cgi-bin/*; do
	cp "$file" /var/www/cgi-bin/
	chmod 755 "/var/www/cgi-bin/${file##*/}"
done
cp "$BASE/runtime-overlay/etc/init.d/S46c120-light-pins" /etc/init.d/
chmod 755 /etc/init.d/S46c120-light-pins
[ -f /etc/c120-light-pins.conf ] || cp "$BASE/runtime-overlay/etc/c120-light-pins.conf" /etc/
[ -f /etc/c120-motion-light.conf ] || cp "$BASE/runtime-overlay/etc/c120-motion-light.conf" /etc/
if ! grep -q 'href="c120-lights.cgi"' "$HEADER"; then
	sed '/href="stream-urls.cgi"/i\
<li><a class="dropdown-item" href="c120-lights.cgi">C120 Lights</a></li>' "$HEADER" > "$HEADER.new"
	chmod 755 "$HEADER.new"
	mv "$HEADER.new" "$HEADER"
fi
/etc/init.d/S46c120-light-pins start
sync
echo "C120 light plugin installed; backup: $BACKUP"

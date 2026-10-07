#!/bin/sh
set -eu

[ "$(id -u)" = 0 ] || { echo "Run as root" >&2; exit 1; }
BASE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HEADER=/var/www/cgi-bin/p/header.cgi
[ -f "$HEADER" ] || { echo "Install the current OpenIPC Web UI first" >&2; exit 1; }
grep -q 'href="stream-urls.cgi"' "$HEADER" || { echo "Unsupported Web UI navigation" >&2; exit 1; }
[ ! -f /run/c120-setup-ap.active ] || { echo "Stop the setup AP first" >&2; exit 1; }
[ -x /etc/init.d/S45c120-ap-button ] || { echo "Install the AP recovery plugin first" >&2; exit 1; }
(cd "$BASE" && sha256sum -c eventd.sha256)

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
URLS=/var/www/cgi-bin/stream-urls.cgi
if [ -f "$URLS" ] && ! grep -q 'c120-floodlight.cgi' "$URLS"; then
	grep -q '<dd>Toggle camera light\.</dd>' "$URLS" || { echo "Unsupported endpoint list" >&2; exit 1; }
	cp -p "$URLS" "$BACKUP/stream-urls.cgi"
	sed "/<dd>Toggle camera light\\.<\/dd>/r $BASE/floodlight-url.html" "$URLS" > "$URLS.new"
	chmod 755 "$URLS.new"
	mv "$URLS.new" "$URLS"
fi
LIVE=/var/www/cgi-bin/live.cgi
if [ -f "$LIVE" ] && ! grep -q 'c120-live-audio.js' "$LIVE"; then
	grep -q '<script src="/a/preview-health.js"></script>' "$LIVE" || { echo "Unsupported live player" >&2; exit 1; }
	cp -p "$LIVE" "$BACKUP/live.cgi"
	sed "/<script src=\"\/a\/preview-health.js\"><\/script>/r $BASE/live-audio.html" "$LIVE" > "$LIVE.new"
	chmod 755 "$LIVE.new"
	mv "$LIVE.new" "$LIVE"
fi
mkdir -p /var/www/a
[ -f /etc/c120-live-audio.json ] || cp "$BASE/runtime-overlay/etc/c120-live-audio.json" /etc/
ln -sf /etc/c120-live-audio.json /var/www/a/c120-live-audio.json
for file in "$BASE"/runtime-overlay/var/www/a/*; do
	cp "$file" /var/www/a/
	chmod 644 "/var/www/a/${file##*/}"
done
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
<li><a class="dropdown-item" href="c120-lights.cgi">Lights</a></li>' "$HEADER" > "$HEADER.new"
	chmod 755 "$HEADER.new"
	mv "$HEADER.new" "$HEADER"
fi
sed -i 's/>C120 Lights</>Lights</g' "$HEADER"
/etc/init.d/S46c120-light-pins start
sync
echo "C120 light plugin installed; backup: $BACKUP"

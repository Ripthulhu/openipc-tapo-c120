#!/bin/sh
set -eu

PLUGIN_NAME="openipc-c120-ap-recovery"
VERSION="2026.05.21"
HOSTAPD_SHA256="a856e3ed876757580e24a3e7bd748c2d8e5da0176fae040191e64dd634b16d52"
BACKUP_DIR="/root/c120-ap-recovery-backups"

fail() {
	echo "ERROR: $*" >&2
	exit 1
}

say() {
	echo "[$PLUGIN_NAME] $*"
}

script_dir() {
	case "$0" in
		*/*)
			dir=${0%/*}
			;;
		*)
			dir=.
			;;
	esac
	cd "$dir" && pwd
}

require_root() {
	uid="$(id -u 2>/dev/null || echo 1)"
	[ "$uid" = "0" ] || fail "run this installer as root"
}

check_payload() {
	[ -d "$BASE/files" ] || fail "missing files/ payload directory"
	[ -f "$BASE/hostapd-overlay.tgz" ] || fail "missing hostapd-overlay.tgz"

	if command -v sha256sum >/dev/null 2>&1; then
		actual="$(sha256sum "$BASE/hostapd-overlay.tgz" | awk '{print $1}')"
		[ "$actual" = "$HOSTAPD_SHA256" ] || fail "hostapd overlay hash mismatch: $actual"
	fi
}

install_file() {
	src="$1"
	dest="$2"
	mode="$3"

	[ -f "$src" ] || fail "missing payload file $src"
	mkdir -p "$(dirname "$dest")"
	cp "$src" "$dest"
	chmod "$mode" "$dest"
}

extract_tgz() {
	archive="$1"
	dest="$2"

	if command -v gzip >/dev/null 2>&1; then
		gzip -dc "$archive" | tar -xf - -C "$dest"
	elif command -v zcat >/dev/null 2>&1; then
		zcat "$archive" | tar -xf - -C "$dest"
	else
		fail "gzip or zcat is required to extract $archive"
	fi
}

backup_wlan0() {
	mkdir -p "$BACKUP_DIR"
	ts="$(date '+%Y%m%d-%H%M%S' 2>/dev/null || echo now)-$$"

	if [ -e /etc/network/interfaces.d/wlan0 ]; then
		cp -p /etc/network/interfaces.d/wlan0 "$BACKUP_DIR/wlan0.$ts"
		say "backed up existing wlan0 config to $BACKUP_DIR/wlan0.$ts"
	fi

	for f in /etc/network/interfaces.d/wlan0.* /etc/network/interfaces.d/*.before-c120-ap; do
		[ -e "$f" ] || continue
		name="${f##*/}"
		mv "$f" "$BACKUP_DIR/$name.$ts" 2>/dev/null || rm -f "$f"
		say "moved parsed backup file out of interfaces.d: $name"
	done
}

stop_watcher() {
	if [ -f /run/c120-setup-ap.active ]; then
		fail "setup AP is active; stop it before updating the plugin"
	fi

	if [ -x /etc/init.d/S45c120-ap-button ]; then
		/etc/init.d/S45c120-ap-button stop >/dev/null 2>&1 || true
	fi
	[ -s /run/c120-eventd.pid ] && kill "$(cat /run/c120-eventd.pid)" 2>/dev/null || true
	[ -s /run/c120-light-pinsd.pid ] && kill "$(cat /run/c120-light-pinsd.pid)" 2>/dev/null || true
	killall -q c120-eventd 2>/dev/null || true
	killall -q c120-button-apd 2>/dev/null || true
	killall -q c120-light-pinsd 2>/dev/null || true
	rm -f /run/c120-eventd.pid /run/c120-button-apd.pid /run/c120-light-pinsd.pid
}

start_watcher() {
	if [ -x /etc/init.d/S45c120-ap-button ]; then
		/etc/init.d/S45c120-ap-button restart >/dev/null 2>&1 || /etc/init.d/S45c120-ap-button start
	fi
}

install_plugin() {
	require_root
	check_payload

	say "installing version $VERSION"
	stop_watcher

	say "installing hostapd/libnl overlay"
	extract_tgz "$BASE/hostapd-overlay.tgz" /
	chmod 0755 /usr/sbin/hostapd /usr/bin/hostapd_cli 2>/dev/null || true

	install_file "$BASE/files/usr/bin/c120-setup-ap" /usr/bin/c120-setup-ap 0755
	install_file "$BASE/files/usr/bin/c120-eventd" /usr/bin/c120-eventd 0755
	install_file "$BASE/files/usr/bin/c120-button-apd" /usr/bin/c120-button-apd 0755
	install_file "$BASE/files/usr/bin/c120-wifi-conf" /usr/bin/c120-wifi-conf 0755
	install_file "$BASE/files/etc/init.d/S45c120-ap-button" /etc/init.d/S45c120-ap-button 0755
	install_file "$BASE/files/var/www/cgi-bin/c120-wifi-setup.cgi" /var/www/cgi-bin/c120-wifi-setup.cgi 0755

	mkdir -p /usr/share/c120-ap-recovery
	install_file "$BASE/manifest.txt" /usr/share/c120-ap-recovery/manifest.txt 0644
	install_file "$BASE/README.md" /usr/share/c120-ap-recovery/README.md 0644

	backup_wlan0
	install_file "$BASE/files/etc/network/interfaces.d/wlan0" /etc/network/interfaces.d/wlan0 0644

	start_watcher
	sync
	say "installed"
	status_plugin
}

status_plugin() {
	echo "version=$VERSION"
	if command -v hostapd >/dev/null 2>&1; then
		hostapd -v 2>&1 | head -n 1 || true
	else
		echo "hostapd=missing"
	fi

	if [ -x /usr/bin/c120-setup-ap ]; then
		/usr/bin/c120-setup-ap status || true
	else
		echo "setup-ap-helper=missing"
	fi

	if [ -x /usr/bin/c120-eventd ]; then
		/usr/bin/c120-eventd status || true
	elif [ -s /run/c120-button-apd.pid ] && kill -0 "$(cat /run/c120-button-apd.pid)" 2>/dev/null; then
		echo "eventd=legacy-button-watcher-running"
	else
		echo "eventd=stopped"
	fi
}

BASE="$(script_dir)"

case "${1:-install}" in
	install)
		install_plugin
		;;
	status)
		status_plugin
		;;
	*)
		echo "Usage: $0 [install|status]" >&2
		exit 1
		;;
esac

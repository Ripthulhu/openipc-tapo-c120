#!/bin/sh
# Temporary key-only SSH, independent of the stock root filesystem. No flash writes.
set -eu

STAGE=${1:-/tmp/mnt/harddisk_1/openipc-c120}
STAGE=$(cd "$STAGE" && pwd)
ROOT=/tmp/c120-rescue
bb() { "$STAGE/libc.so" "$STAGE/busybox" "$@"; }

cd "$STAGE"
bb sha256sum -c rescue.sha256
[ ! -e "$ROOT" ] || { echo "rescue directory already exists" >&2; exit 1; }
umask 077
bb mkdir "$ROOT"
bb gzip -dc rescue-runtime.tgz | bb tar -xf - -C "$ROOT"
bb mkdir -p "$ROOT/etc" "$ROOT/root/.ssh" "$ROOT/tmp" "$ROOT/var/run" \
    "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/stage" "$ROOT/host"
printf '%s\n' 'root:x:0:0:Recovery:/root:/bin/sh' > "$ROOT/etc/passwd"
printf '%s\n' 'root:x:0:0:99999:7:::' > "$ROOT/etc/shadow"
printf '%s\n' 'root:x:0:' > "$ROOT/etc/group"
printf '%s\n' '/bin/sh' > "$ROOT/etc/shells"
bb cp recovery.pub "$ROOT/root/.ssh/authorized_keys"
bb chmod 600 "$ROOT/root/.ssh/authorized_keys" "$ROOT/etc/shadow"
for fs in dev proc sys; do
    bb mount -o bind "/$fs" "$ROOT/$fs"
done
bb mount -o bind "$STAGE" "$ROOT/stage"
bb mount -o bind / "$ROOT/host"
bb chroot "$ROOT" /usr/bin/dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key
bb chroot "$ROOT" /usr/bin/dropbearkey -y -f /etc/dropbear_ed25519_host_key > "$ROOT/hostkey.pub"
"$STAGE/rescue-guard" exec "$STAGE/libc.so" "$STAGE/busybox" \
    chroot "$ROOT" /usr/sbin/dropbear -E -s -g -p 2222 \
    -r /etc/dropbear_ed25519_host_key -P /var/run/dropbear.pid > "$ROOT/dropbear.log" 2>&1
echo "Temporary key-only recovery SSH started on port 2222"

#!/bin/sh
# Used only from the authenticated, key-pinned RAM recovery session.
set -eu
BB=/bin/busybox
STATE=/var/run/stock-pids

case "${1:-}" in
    --resume)
        [ -f "$STATE" ] || exit 0
        while read -r mountpoint; do
            $BB mount -t jffs2 -o remount,rw /dev/mtdblock8 "/proc/1/root$mountpoint"
        done < /var/run/stock-mounts
        for pid in $($BB cat "$STATE"); do
            case "$($BB readlink /proc/$pid/exe)" in
                /bin/main|/bin/monitor) kill -CONT "$pid" ;;
                *) echo "Stock PID changed; leave recovery connected" >&2; exit 1 ;;
            esac
        done
        echo "STOCK_RESUMED"
        exit 0 ;;
    --freeze) ;;
    *) exit 2 ;;
esac

[ ! -e "$STATE" ] || { echo "Already quiesced; do not repeat" >&2; exit 1; }
guard=$($BB cat /var/run/watchdog.pid)
kill -0 "$guard"
[ "$($BB readlink /proc/$guard/fd/6)" = /dev/watchdog ]
$BB grep -q rescue-guard /proc/$guard/cmdline
pids=
for entry in /proc/[0-9]*/exe; do
    case "$($BB readlink "$entry" 2>/dev/null || :)" in
        /bin/main|/bin/monitor)
            pid=${entry%/exe}; pids="$pids ${pid##*/}" ;;
    esac
done
set -- $pids
[ "$#" -eq 2 ] || { echo "Expected exactly two stock services" >&2; exit 1; }
$BB awk '$1 ~ /^\/dev\/mtdblock[0-9]+$/ && $3 == "jffs2" && $4 ~ /(^|,)rw(,|$)/ {print $2}' \
    /proc/1/mounts > /var/run/stock-mounts
for mountpoint in $($BB cat /var/run/stock-mounts); do
    case "$mountpoint" in /tmp/usr_def_audio) ;; *) echo "Unexpected flash mount" >&2; exit 1 ;; esac
done
printf '%s\n' "$pids" > "$STATE"
for pid in $pids; do kill -STOP "$pid"; done
$BB sync
while read -r mountpoint; do
    # /host is a non-recursive bind: use init's root to reach its child mounts.
    $BB mount -t jffs2 -o remount,ro /dev/mtdblock8 "/proc/1/root$mountpoint"
done < /var/run/stock-mounts
# Stock watchdog on the supported firmware is 10 seconds. Observe two periods.
$BB sleep 22
kill -0 "$guard"
echo "STOCK_QUIESCED"

#!/bin/sh
# Run only inside the RAM recovery chroot after freezing stock services.
set -eu

BB=/bin/busybox
STAGE=/stage
WRITER=$STAGE/mtdw-physical
MODE=${1:-}
case "$MODE:$#" in
    --preflight:3) MAC=$2 SENSOR=$3 ;;
    --flash:4)
        [ "$2" = --yes-i-understand ] || exit 2
        MAC=$3 SENSOR=$4 ;;
    *) echo "usage: $0 --preflight MAC SENSOR | --flash --yes-i-understand MAC SENSOR" >&2; exit 2 ;;
esac
case "$SENSOR" in
    sc438hai)
        MODULE=drv_ms_cus_sc438hai_2lane
        MAIN_SHA=ab1dd0d2ab8f2f29de17185dd0460fe4a28961008f6a433684f9eb4065f6803d ;;
    sc430ai)
        MODULE=drv_ms_cus_sc430ai_MIPI_tp_ww
        MAIN_SHA=6ea2fd02fa952dd998e433405c797a4b7686714f9eafabcd898e91dfa38adbef ;;
    *) echo "Unsupported sensor" >&2; exit 2 ;;
esac

die() { echo "$*" >&2; exit 1; }

preflight() {
    [ "$($BB readlink /proc/self/root)" = / ] || die "Unexpected recovery root"
    [ -f /etc/dropbear_ed25519_host_key ] || die "Recovery runtime missing"
    [ "$($BB cat /sys/class/net/wlan0/address)" = "$MAC" ] || die "Wrong camera MAC"
    $BB grep -q "^$MODULE " /proc/modules || die "Wrong sensor module"
    [ "$($BB sha256sum /host/bin/main | $BB cut -d ' ' -f 1)" = "$MAIN_SHA" ] || die "Wrong stock application"
    $BB grep -qx 'mtd0: 00030000 00001000 "factory_boot"' /proc/mtd || die "Wrong boot partition"
    $BB grep -qx 'mtd1: 00010000 00001000 "factory_info"' /proc/mtd || die "Wrong boot tail partition"
    $BB grep -qx 'mtd15: 00fc0000 00001000 "af"' /proc/mtd || die "Wrong aggregate partition"
    if $BB grep -E 'mtdblock[0-9]+ .* (jffs2|squashfs) rw[, ]' /proc/1/mounts; then
        die "Writable flash filesystem still mounted"
    fi
    guard=$($BB cat /var/run/watchdog.pid)
    kill -0 "$guard" || die "Recovery watchdog is absent"
    [ "$($BB readlink /proc/$guard/fd/6)" = /dev/watchdog ] || die "Watchdog descriptor missing"
    $BB grep -q rescue-guard /proc/$guard/cmdline || die "Unexpected watchdog process"
    found=0
    for entry in /proc/[0-9]*/exe; do
        case "$($BB readlink "$entry" 2>/dev/null || :)" in
            /bin/main|/bin/monitor)
                $BB grep -q '^State:.*T (stopped)' "${entry%exe}status" || die "Stock service not frozen"
                found=$((found + 1)) ;;
        esac
    done
    [ "$found" -eq 2 ] || die "Expected both frozen stock services"
    available=$($BB awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    [ "$available" -ge 4096 ] || die "Insufficient recovery memory"
    for file in openipc-raw.bin raw-stock-quiesced.bin; do
        [ "$($BB stat -c %s "$STAGE/$file")" -eq 16777216 ] || die "Wrong image size: $file"
    done
    cd "$STAGE"
    $BB sha256sum -c physical-flash.sha256 || die "Staged file checksum failure"
    "$WRITER" stock-compare 0 "$STAGE/raw-stock-quiesced.bin" || die "Stock backup differs from physical flash"
    echo "PREFLIGHT_OK"
}

# All slices are multiples of 64 KiB; the writer verifies through the vendor
# physical-read ioctl, not the stock MTD reader's substituted rootfs header.
write_slice() {
    file=$1 skip=$2 count=$3 mtd=$4 base=$5 offset=$6 size=$7
    $BB dd if="$STAGE/$file" bs=65536 skip="$skip" count="$count" |
        "$WRITER" stock-write-stream "$mtd" "$base" "$offset" "$size" "$size"
}

upper_changed=0
first_changed=0
install_image() {
    echo "WRITING_OPENIPC_BODY"
    write_slice openipc-raw.bin 5 251 /dev/mtd15 0x40000 0x10000 0xfb0000 || return 1
    echo "WRITING_OPENIPC_ENV"
    write_slice openipc-raw.bin 4 1 /dev/mtd15 0x40000 0 0x10000 || return 1
    echo "WRITING_OPENIPC_BOOT_TAIL"
    upper_changed=1
    write_slice openipc-raw.bin 3 1 /dev/mtd1 0x30000 0 0x10000 || return 1
    echo "WRITING_OPENIPC_BOOT"
    first_changed=1
    write_slice openipc-raw.bin 0 3 /dev/mtd0 0 0 0x30000 || return 1
    "$WRITER" stock-compare 0 "$STAGE/openipc-raw.bin"
}

restore_stock() {
    echo "RESTORING_COMPLETE_STOCK"
    write_slice raw-stock-quiesced.bin 4 252 /dev/mtd15 0x40000 0 0xfc0000 || return 1
    if [ "$upper_changed" -eq 1 ]; then
        write_slice raw-stock-quiesced.bin 3 1 /dev/mtd1 0x30000 0 0x10000 || return 1
    fi
    if [ "$first_changed" -eq 1 ]; then
        write_slice raw-stock-quiesced.bin 0 3 /dev/mtd0 0 0 0x30000 || return 1
    fi
    "$WRITER" stock-compare 0 "$STAGE/raw-stock-quiesced.bin"
}

preflight
[ "$MODE" = --flash ] || exit 0
if install_image; then
    echo "OPENIPC_PHYSICAL_VERIFY_OK"
elif restore_stock; then
    echo "STOCK_ROLLBACK_PHYSICAL_VERIFY_OK"
else
    die "ROLLBACK_FAILED: DO NOT REBOOT; recovery SSH and watchdog remain active"
fi
$BB sync
$BB sleep 3
$BB reboot -f

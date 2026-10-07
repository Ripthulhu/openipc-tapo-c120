#!/bin/sh
set -eu
[ "$(id -u)" = 0 ] || { echo "Run as root" >&2; exit 1; }
case "$(uname -m)" in arm*) ;; *) echo "ARM hard-float OpenIPC required" >&2; exit 1 ;; esac
[ -e /lib/ld-musl-armhf.so.1 ] || { echo "The musl OpenIPC runtime is required" >&2; exit 1; }
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
(cd "$src" && sha256sum -c SHA256SUMS)
# No service or configuration changes: this is an on-demand encoder only.
for path in usr/bin/lame usr/lib/libmp3lame.so.0 usr/share/licenses/lame/COPYING; do
    if [ -e "/$path" ] || [ -L "/$path" ]; then
        cmp -s "$src/$path" "/$path" || {
            echo "Existing /$path differs; back it up before replacing it" >&2; exit 1;
        }
    fi
done
required=$(du -sk "$src/usr" | awk '{print $1+512}')
free=$(df -Pk /overlay | tail -n 1 | awk '{print $4}')
[ "$free" -ge "$required" ] || { echo "Insufficient flash headroom" >&2; exit 1; }
chmod 755 "$src/usr/bin/lame"
LD_LIBRARY_PATH="$src/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$src/usr/bin/lame" --version >/dev/null
for path in usr/bin/lame usr/lib/libmp3lame.so.0 usr/share/licenses/lame/COPYING; do
    [ ! -e "/$path" ] || continue
    mkdir -p "$(dirname "/$path")"
    cp "$src/$path" "/$path.mp3-new"
    case "$path" in usr/bin/lame) chmod 755 "/$path.mp3-new" ;; *) chmod 644 "/$path.mp3-new" ;; esac
    mv "/$path.mp3-new" "/$path"
done
sync
/usr/bin/lame --version
echo "MP3 tools installed; microphone settings and running services were left unchanged."

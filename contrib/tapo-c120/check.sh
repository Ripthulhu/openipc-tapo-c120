#!/bin/sh
# Host-only checks: no cameras, credentials or proprietary models are needed.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PYTHONDONTWRITEBYTECODE=1

for tool in gcc make haserl node python3 pkg-config ffmpeg curl unsquashfs; do
    command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 1; }
done
pkg-config --exists libcurl json-c ogg opus zlib || {
    echo "Install the libcurl, json-c, libogg, libopus and zlib development packages" >&2
    exit 1
}
python3 -c 'import numpy'

python3 "$base/test_runtime.py"
python3 "$base/test_qhd.py"
python3 "$base/test_sc438hai.py"
node "$base/test_live_audio.cjs"
node --check "$base/ai-plugin/files/var/www/a/c120-ai.js"
python3 "$base/ai-plugin/test_actions.py"
python3 "$base/ai-plugin/test_sound.py"
python3 "$base/ai-plugin/test_models.py"
python3 "$base/stock-install/test_c120_install.py"
python3 "$base/stock-install/test_tapo_client.py"
python3 "$base/stock-install/test_c120_tftp.py"
python3 "$base/stock-install/test_c120_prepare_raw.py"
python3 "$base/stock-install/test_c120_packed_images.py"
python3 "$base/stock-install/test_c120_stock_recovery_flash.py"
python3 "$base/stock-install/test_c120_rescue_guard.py"
python3 "$base/../../general/scripts/tests/test_rootfs_startup.py"
python3 "$base/stock-install/c120_install.py" --check-kit

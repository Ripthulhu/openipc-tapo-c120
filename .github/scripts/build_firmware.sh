#!/bin/bash
set -eu

board=${1:?usage: build_firmware.sh BOARD}
GIT_HASH=$(git rev-parse --short HEAD)
export GIT_HASH
export GIT_BRANCH=${GITHUB_REF_NAME}
echo "GIT_HASH=${GIT_HASH}" >> "${GITHUB_ENV}"
echo "GIT_BRANCH=${GIT_BRANCH}" >> "${GITHUB_ENV}"

mkdir -p /tmp/ccache
ln -s /tmp/ccache "${HOME}/.ccache"

# Seven attempts cover extended toolchain mirror and release CDN outages.
backoffs="30 60 120 300 600 1200"
attempt=1
for sleep_for in $backoffs ""; do
	make "BOARD=$board" && break
	if [ -z "$sleep_for" ]; then
		echo "::error::build failed after ${attempt} attempts"
		exit 1
	fi
	echo "::warning::attempt ${attempt} failed, retrying after ${sleep_for}s"
	sleep "$sleep_for"
	attempt=$((attempt + 1))
done

TIME=$(date -d @${SECONDS} +%M:%S)
echo "TIME=${TIME}" >> "${GITHUB_ENV}"

NORFW=$(find output/images -name 'openipc*nor*')
if [ -e "$NORFW" ]; then
	echo "NORFW=${NORFW}" >> "${GITHUB_ENV}"
fi

NANDFW=$(find output/images -name 'openipc*nand*')
if [ -e "$NANDFW" ]; then
	echo "NANDFW=${NANDFW}" >> "${GITHUB_ENV}"
fi

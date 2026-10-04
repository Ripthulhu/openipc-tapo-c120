#!/bin/bash
DATE=$(date +%y.%m.%d)
FILE=${TARGET_DIR}/usr/lib/os-release

echo OPENIPC_VERSION=${DATE:0:1}.${DATE:1} >> ${FILE}
date +GITHUB_VERSION="\"${GIT_BRANCH-local}+${GIT_HASH-build}, %Y-%m-%d"\" >> ${FILE}
echo BUILD_OPTION=${OPENIPC_VARIANT} >> ${FILE}
echo BUILD_ID=${BUILD_ID:-local-$(date -u +%Y%m%d)-${GIT_HASH-build}} >> ${FILE}
echo BUILD_SHA=${BUILD_SHA:-${GIT_HASH-build}} >> ${FILE}
echo BUILD_PLATFORM=${BUILD_PLATFORM:-${OPENIPC_SOC_MODEL}_${OPENIPC_VARIANT}} >> ${FILE}
date +TIME_STAMP=%s >> ${FILE}

CONF="USES_GLIBC=y|OSDRV_T30=y|OSDRV_V85X=y|LIBV4L=y|MAVLINK_ROUTER=y|RUBYFPV=y|ONYXFPV=y|WIFIBROADCAST=y|WIFIBROADCAST_NG=y|AUDIO_PROCESSING_OPENIPC=y"
if ! grep -qP ${CONF} ${BR2_CONFIG}; then
	rm -f ${TARGET_DIR}/usr/lib/libstdc++*
fi

if grep -q "USES_MUSL=y" ${BR2_CONFIG}; then
	ln -sf libc.so ${TARGET_DIR}/lib/ld-uClibc.so.0
	ln -sf ../../lib/libc.so ${TARGET_DIR}/usr/bin/ldd

	# Keep libraries referenced by either ELF NEEDED entries or literal dlopen names.
	for lib in libgcc_s libatomic; do
		if grep -rqaF -D skip --exclude="${lib}.so*" "${lib}.so" "${TARGET_DIR}"; then
			continue
		else
			case $? in
				1) rm -f "${TARGET_DIR}"/lib/${lib}.so* "${TARGET_DIR}"/usr/lib/${lib}.so* ;;
				*) echo "Cannot check references to ${lib}; refusing to prune" >&2; exit 1 ;;
			esac
		fi
	done
fi

# BusyBox reads text module indexes; preserve binary indexes when kmod is present.
if [ -z "$(find "${TARGET_DIR}/bin" "${TARGET_DIR}/sbin" "${TARGET_DIR}/usr/bin" "${TARGET_DIR}/usr/sbin" -name kmod -type f 2>/dev/null)" ]; then
	rm -f "${TARGET_DIR}"/lib/modules/*/modules.*.bin "${TARGET_DIR}"/lib/modules/*/modules.builtin.modinfo
fi

LIST="${BR2_EXTERNAL_GENERAL_PATH}/scripts/excludes/${OPENIPC_SOC_MODEL}_${OPENIPC_VARIANT}.list"
if [ -f "${LIST}" ]; then
	xargs -a "${LIST}" -I % rm -f "${TARGET_DIR}%"
fi

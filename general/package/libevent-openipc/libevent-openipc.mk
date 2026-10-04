################################################################################
#
# libevent-openipc
#
################################################################################

LIBEVENT_OPENIPC_SITE_METHOD = git
LIBEVENT_OPENIPC_SITE = https://github.com/OpenIPC/libevent
# Pinned OpenIPC fork includes the build toggles, musl mmap and WebSocket fixes.
# A moving pull-request ref broke clean builds when its author rebased it.
LIBEVENT_OPENIPC_VERSION = 694decef35717d8955aa34ba4d2baaaf61c9e4a9

LIBEVENT_OPENIPC_INSTALL_STAGING = YES
LIBEVENT_OPENIPC_LICENSE = BSD-3-Clause, OpenBSD
LIBEVENT_OPENIPC_LICENSE_FILES = LICENSE
LIBEVENT_OPENIPC_CONF_OPTS = \
	-D_GNU_SOURCE=ON \
	-DEVENT__DISABLE_BENCHMARK=ON \
	-DEVENT__DISABLE_SAMPLES=ON \
	-DEVENT__DISABLE_TESTS=ON \
	-DEVENT__DISABLE_RPC=ON \
	-DEVENT__DISABLE_EVENT_TAGGING=ON \
	-DCMAKE_BUILD_TYPE=Release

define LIBEVENT_OPENIPC_REMOVE_PYSCRIPT
	rm $(TARGET_DIR)/usr/bin/event_rpcgen.py
endef

define LIBEVENT_OPENIPC_DELETE_UNUSED
	rm -f $(TARGET_DIR)/usr/lib/libevent-2.2.so*
	rm -f $(TARGET_DIR)/usr/lib/libevent.so
endef

# libevent installs a python script to target - get rid of it if we
# don't have python support enabled
ifneq ($(BR2_PACKAGE_PYTHON)$(BR2_PACKAGE_PYTHON3),y)
LIBEVENT_OPENIPC_POST_INSTALL_TARGET_HOOKS += LIBEVENT_OPENIPC_REMOVE_PYSCRIPT
endif

ifeq ($(BR2_PACKAGE_OPENSSL),y)
LIBEVENT_OPENIPC_DEPENDENCIES += host-pkgconf openssl
LIBEVENT_OPENIPC_CONF_OPTS += -DEVENT__DISABLE_OPENSSL=OFF
else
LIBEVENT_OPENIPC_CONF_OPTS += -DEVENT__DISABLE_OPENSSL=ON
endif

ifeq ($(BR2_PACKAGE_MBEDTLS_OPENIPC),y)
LIBEVENT_OPENIPC_DEPENDENCIES += host-pkgconf mbedtls-openipc
LIBEVENT_OPENIPC_CONF_OPTS += -DEVENT__DISABLE_MBEDTLS=OFF
else
LIBEVENT_OPENIPC_CONF_OPTS += -DEVENT__DISABLE_MBEDTLS=ON
endif

ifeq ($(BR2_TOOLCHAIN_USES_MUSL),y)
ifneq ($(BR2_aarch64),y)
LIBEVENT_OPENIPC_CONF_OPTS += -DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -DBROKEN_MMAP=1"
endif
endif

LIBEVENT_OPENIPC_POST_INSTALL_TARGET_HOOKS += LIBEVENT_OPENIPC_DELETE_UNUSED

$(eval $(cmake-package))

################################################################################
#
# c120-qhd
#
################################################################################

C120_QHD_VERSION = 1
C120_QHD_SITE = $(C120_QHD_PKGDIR)/src
C120_QHD_SITE_METHOD = local
C120_QHD_LICENSE = MIT
C120_QHD_LICENSE_FILES = LICENSE

define C120_QHD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC -shared \
		$(@D)/c120-qhd.c $(TARGET_LDFLAGS) -ldl -o $(@D)/libc120-qhd.so
endef

define C120_QHD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 644 $(@D)/libc120-qhd.so $(TARGET_DIR)/usr/lib/libc120-qhd.so
	$(INSTALL) -D -m 644 "$(C120_QHD_PKGDIR)/majestic.env" $(TARGET_DIR)/etc/default/majestic
endef

$(eval $(generic-package))

################################################################################
#
# xmdp
#
################################################################################

XMDP_LICENSE = Public Domain
XMDP_DEPENDENCIES = cjson

define XMDP_EXTRACT_CMDS
	cp -avr $(XMDP_PKGDIR)/src/* $(@D)/
endef

define XMDP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define XMDP_INSTALL_TARGET_CMDS
	install -m 0755 -D $(@D)/xmdp $(TARGET_DIR)/usr/bin/xmdp
endef

$(eval $(generic-package))

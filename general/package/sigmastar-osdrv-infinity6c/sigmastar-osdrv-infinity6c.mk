################################################################################
#
# sigmastar-osdrv-infinity6c
#
################################################################################

SIGMASTAR_OSDRV_INFINITY6C_VERSION =
SIGMASTAR_OSDRV_INFINITY6C_SITE =
SIGMASTAR_OSDRV_INFINITY6C_LICENSE = MIT
SIGMASTAR_OSDRV_INFINITY6C_LICENSE_FILES = LICENSE

define SIGMASTAR_OSDRV_INFINITY6C_INSTALL_TARGET_CMDS
	$(INSTALL) -m 755 -d $(TARGET_DIR)/lib/modules/5.10.61/sigmastar
	$(INSTALL) -m 644 -t $(TARGET_DIR)/lib/modules/5.10.61/sigmastar $(SIGMASTAR_OSDRV_INFINITY6C_PKGDIR)/files/kmod/*

	$(INSTALL) -m 755 -d $(TARGET_DIR)/etc/firmware
	$(INSTALL) -m 644 -t $(TARGET_DIR)/etc/firmware $(SIGMASTAR_OSDRV_INFINITY6C_PKGDIR)/files/sensor/firmware/*

	$(INSTALL) -m 755 -d $(TARGET_DIR)/etc/sensors
	if [ -n "$(OPENIPC_SNS_MODEL)" ]; then \
		if [ -f "$(SIGMASTAR_OSDRV_INFINITY6C_PKGDIR)/files/sensor/configs/$(OPENIPC_SNS_MODEL).bin" ]; then \
			$(INSTALL) -m 644 -t $(TARGET_DIR)/etc/sensors $(SIGMASTAR_OSDRV_INFINITY6C_PKGDIR)/files/sensor/configs/$(OPENIPC_SNS_MODEL).bin; \
		else \
			echo "No IQ/config blob for sensor $(OPENIPC_SNS_MODEL); continuing without /etc/sensors/$(OPENIPC_SNS_MODEL).bin"; \
		fi; \
	else \
		$(INSTALL) -m 644 -t $(TARGET_DIR)/etc/sensors $(SIGMASTAR_OSDRV_INFINITY6C_PKGDIR)/files/sensor/configs/*.bin; \
	fi

	$(INSTALL) -m 755 -d $(TARGET_DIR)/usr/bin
	$(INSTALL) -m 755 -t $(TARGET_DIR)/usr/bin $(SIGMASTAR_OSDRV_INFINITY6C_PKGDIR)/files/script/*
endef

define SIGMASTAR_OSDRV_INFINITY6C_LIBRARIES
	$(INSTALL) -m 755 -d $(TARGET_DIR)/usr/lib
	$(INSTALL) -m 644 -t $(TARGET_DIR)/usr/lib $(SIGMASTAR_OSDRV_INFINITY6C_PKGDIR)/files/lib/*
endef

ifneq ($(BR2_PACKAGE_MAJESTIC),y)
SIGMASTAR_OSDRV_INFINITY6C_POST_INSTALL_TARGET_HOOKS += SIGMASTAR_OSDRV_INFINITY6C_LIBRARIES
endif

$(eval $(generic-package))

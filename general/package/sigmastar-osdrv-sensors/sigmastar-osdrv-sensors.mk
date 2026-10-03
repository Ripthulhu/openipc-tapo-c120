################################################################################
#
# sigmastar-osdrv-sensors
#
################################################################################

SIGMASTAR_OSDRV_SENSORS_SITE = $(call github,openipc,sensors,$(SIGMASTAR_OSDRV_SENSORS_VERSION))
SIGMASTAR_OSDRV_SENSORS_VERSION = HEAD

SIGMASTAR_OSDRV_SENSORS_MODULE_SUBDIRS = $(OPENIPC_SOC_VENDOR)/$(OPENIPC_SOC_FAMILY)
SIGMASTAR_OSDRV_SENSORS_MODULE_MAKE_OPTS = \
	SENSOR_VERSION=$(OPENIPC_SOC_FAMILY) \
	INSTALL_MOD_DIR=$(OPENIPC_SOC_VENDOR) \
	KSRC=$(LINUX_DIR)

define SIGMASTAR_OSDRV_SENSORS_COPY_LOCAL_SOURCES
	if [ -n "$(OPENIPC_SNS_MODEL)" ] && \
		[ -f "$(BR2_EXTERNAL)/../sigmastar/$(OPENIPC_SOC_FAMILY)/sensor_$(OPENIPC_SNS_MODEL)_mipi.c" ]; then \
		mkdir -p "$(@D)/$(SIGMASTAR_OSDRV_SENSORS_MODULE_SUBDIRS)"; \
		cp "$(BR2_EXTERNAL)/../sigmastar/$(OPENIPC_SOC_FAMILY)/sensor_$(OPENIPC_SNS_MODEL)_mipi.c" \
			"$(@D)/$(SIGMASTAR_OSDRV_SENSORS_MODULE_SUBDIRS)/"; \
	fi
endef
SIGMASTAR_OSDRV_SENSORS_POST_PATCH_HOOKS += SIGMASTAR_OSDRV_SENSORS_COPY_LOCAL_SOURCES

$(eval $(kernel-module))
$(eval $(generic-package))

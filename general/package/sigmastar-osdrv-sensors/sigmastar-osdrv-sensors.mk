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
	if [ -d "$(BR2_EXTERNAL)/../sigmastar" ]; then \
		cp -a "$(BR2_EXTERNAL)/../sigmastar/." "$(@D)/"; \
	fi
endef
SIGMASTAR_OSDRV_SENSORS_POST_PATCH_HOOKS += SIGMASTAR_OSDRV_SENSORS_COPY_LOCAL_SOURCES

$(eval $(kernel-module))
$(eval $(generic-package))

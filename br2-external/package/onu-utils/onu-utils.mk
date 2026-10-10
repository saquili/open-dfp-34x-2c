# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# onu-utils, built from the onu-utils/ directory of this repository.

ONU_UTILS_VERSION = local
ONU_UTILS_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../onu-utils
ONU_UTILS_SITE_METHOD = local
ONU_UTILS_LICENSE = GPL-2.0-only
ONU_UTILS_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt
ONU_UTILS_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES
ONU_UTILS_DEPENDENCIES = libmnl zlib

define ONU_UTILS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define ONU_UTILS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/ponid $(TARGET_DIR)/usr/sbin/ponid
	$(INSTALL) -D -m 0755 $(@D)/build/pon $(TARGET_DIR)/usr/sbin/pon
	$(INSTALL) -D -m 0755 $(@D)/build/onucfg $(TARGET_DIR)/usr/sbin/onucfg
	$(INSTALL) -D -m 0755 $(@D)/build/onu-update $(TARGET_DIR)/usr/sbin/onu-update
	$(INSTALL) -D -m 0755 $(@D)/scripts/onu-trial $(TARGET_DIR)/usr/sbin/onu-trial
	$(INSTALL) -D -m 0755 $(@D)/scripts/onu-mode $(TARGET_DIR)/usr/sbin/onu-mode
	$(INSTALL) -D -m 0755 $(@D)/scripts/onu-store $(TARGET_DIR)/usr/sbin/onu-store
	$(INSTALL) -D -m 0755 $(@D)/scripts/onu-lane $(TARGET_DIR)/usr/sbin/onu-lane
	$(INSTALL) -D -m 0755 $(@D)/scripts/onu-mgmt $(TARGET_DIR)/usr/sbin/onu-mgmt
	$(INSTALL) -D -m 0755 $(@D)/scripts/onu-net $(TARGET_DIR)/usr/sbin/onu-net
	$(INSTALL) -D -m 0755 $(@D)/scripts/passwd $(TARGET_DIR)/usr/bin/passwd
	$(INSTALL) -D -m 0755 $(@D)/scripts/dhcp-wan $(TARGET_DIR)/usr/share/onu/dhcp-wan
endef

$(eval $(generic-package))

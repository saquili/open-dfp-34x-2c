# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pond, built from the pond/ directory of this repository.

POND_VERSION = local
POND_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../pond
POND_SITE_METHOD = local
POND_LICENSE = GPL-2.0-only
POND_DEPENDENCIES = libmnl

define POND_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define POND_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/pond $(TARGET_DIR)/usr/sbin/pond
	$(INSTALL) -D -m 0644 $(@D)/www/index.html $(TARGET_DIR)/usr/share/pond/index.html
endef

$(eval $(generic-package))

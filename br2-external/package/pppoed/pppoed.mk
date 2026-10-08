# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pppoed, built from the pppoed/ directory of this repository.

PPPOED_VERSION = local
PPPOED_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../pppoed
PPPOED_SITE_METHOD = local
PPPOED_LICENSE = GPL-2.0-only
PPPOED_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt
PPPOED_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES

define PPPOED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define PPPOED_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/pppoed $(TARGET_DIR)/usr/sbin/pppoed
endef

$(eval $(generic-package))

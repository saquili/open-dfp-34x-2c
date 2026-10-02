# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# onu-utils, built from the onu-utils/ directory of this repository.

ONU_UTILS_VERSION = local
ONU_UTILS_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../onu-utils
ONU_UTILS_SITE_METHOD = local
ONU_UTILS_LICENSE = GPL-2.0-only
ONU_UTILS_DEPENDENCIES = zlib

define ONU_UTILS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define ONU_UTILS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/ponid $(TARGET_DIR)/usr/sbin/ponid
endef

$(eval $(generic-package))

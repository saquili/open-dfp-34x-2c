# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# omcid, built from the omcid/ directory of this repository for the board.

OMCID_VERSION = local
OMCID_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../omcid
OMCID_SITE_METHOD = local
OMCID_LICENSE = GPL-2.0-only
OMCID_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt
OMCID_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES

define OMCID_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) BOARD=1 board \
		CC="$(TARGET_CC)" AR="$(TARGET_AR)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define OMCID_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build-board/board/omcid $(TARGET_DIR)/usr/sbin/omcid
	$(INSTALL) -D -m 0755 $(@D)/build-board/board/omcitap $(TARGET_DIR)/usr/bin/omcitap
	$(INSTALL) -D -m 0755 $(@D)/build-board/board/omcivlan $(TARGET_DIR)/usr/bin/omcivlan
endef

$(eval $(generic-package))

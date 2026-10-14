# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# timed, built from the timed/ directory of this repository.
#
# timed runs as the account ntp (post-build.sh writes it), with the three
# powers it uses (onu-drop): to set the clock, to listen for PTP over layer 2,
# and to listen on PTP's UDP ports 319 and 320.

TIMED_VERSION = local
TIMED_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../timed
TIMED_SITE_METHOD = local
TIMED_LICENSE = GPL-2.0-only
TIMED_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt
TIMED_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES

define TIMED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define TIMED_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/timed $(TARGET_DIR)/usr/sbin/timed
endef

$(eval $(generic-package))

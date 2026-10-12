# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pppoed, built from the pppoed/ directory of this repository.
#
# Every session runs as an account of its own, pppoed (post-build.sh writes
# it), with the two powers it uses (onu-drop): ubusd's access list,
# pppoed/acl.json, lets it tell netifd of its session, and nothing else.

PPPOED_VERSION = local
PPPOED_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../pppoed
PPPOED_SITE_METHOD = local
PPPOED_LICENSE = GPL-2.0-only
PPPOED_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt
PPPOED_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES
PPPOED_DEPENDENCIES = json-c libubox ubus

define PPPOED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

# netifd runs a protocol's handler from /lib/netifd/proto: pppoed is the
# handler of its protocol "pppoed".
define PPPOED_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/pppoed $(TARGET_DIR)/usr/sbin/pppoed
	$(INSTALL) -d $(TARGET_DIR)/lib/netifd/proto
	ln -sf ../../../usr/sbin/pppoed $(TARGET_DIR)/lib/netifd/proto/pppoed.sh
	$(INSTALL) -D -m 0644 $(@D)/acl.json $(TARGET_DIR)/usr/share/acl.d/pppoed.json
endef

$(eval $(generic-package))

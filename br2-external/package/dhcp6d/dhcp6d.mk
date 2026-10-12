# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# dhcp6d, built from the dhcp6d/ directory of this repository.
#
# dhcp6d runs as an account of its own, dhcp6d (post-build.sh writes it),
# with the three powers it uses (onu-drop): ubusd's access list,
# dhcp6d/acl.json, lets it publish its objects and tell onu-apply of a change,
# and nothing else.

DHCP6D_VERSION = local
DHCP6D_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../dhcp6d
DHCP6D_SITE_METHOD = local
DHCP6D_LICENSE = GPL-2.0-only
DHCP6D_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt
DHCP6D_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES
DHCP6D_DEPENDENCIES = libubox ubus

define DHCP6D_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define DHCP6D_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/dhcp6d $(TARGET_DIR)/usr/sbin/dhcp6d
	$(INSTALL) -D -m 0644 $(@D)/acl.json $(TARGET_DIR)/usr/share/acl.d/dhcp6d.json
endef

$(eval $(generic-package))

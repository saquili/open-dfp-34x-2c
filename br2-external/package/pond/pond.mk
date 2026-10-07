# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pond, built from the pond/ directory of this repository: the daemon, and its
# panel, which Buildroot's host Node builds from pond/web. npm ci installs the
# panel's packages as package-lock.json pins them, with their checksums.

POND_VERSION = local
POND_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../pond
POND_SITE_METHOD = local
POND_LICENSE = GPL-2.0-only
POND_DEPENDENCIES = libmnl host-nodejs
POND_NPM = PATH=$(HOST_DIR)/bin:$$PATH npm_config_cache=$(@D)/.npm \
	npm_config_update_notifier=false npm --prefix $(@D)/web

define POND_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
	$(POND_NPM) ci --no-audit --no-fund
	$(POND_NPM) run build
endef

define POND_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/pond $(TARGET_DIR)/usr/sbin/pond
	rm -rf $(TARGET_DIR)/usr/share/pond
	mkdir -p $(TARGET_DIR)/usr/share/pond/www
	cp -R $(@D)/web/dist/. $(TARGET_DIR)/usr/share/pond/www/
	chmod -R u=rwX,go=rX $(TARGET_DIR)/usr/share/pond/www
endef

$(eval $(generic-package))

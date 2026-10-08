# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pond, built from the pond/ directory of this repository: the daemon, and its
# panel, which Buildroot's host Node builds from pond/web. npm ci installs the
# panel's packages as package-lock.json pins them, with their checksums.
#
# The daemon holds Mongoose, which holds micro-ecc under BSD-2-Clause. The
# panel holds code of packages under MIT and ISC; its build writes their
# licence texts into licenses.txt, which goes into the image with the panel's
# files (pond/web/licences.ts).

POND_VERSION = local
POND_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../pond
POND_SITE_METHOD = local
POND_LICENSE = GPL-2.0-only, BSD-2-Clause (micro-ecc in Mongoose), MIT, ISC (packages of the panel)
POND_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt LICENSES/preferred/BSD-2-Clause.txt
POND_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES
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

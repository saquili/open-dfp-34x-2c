# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# pond, built from the pond/ directory of this repository: the daemon, and its
# panel, which Buildroot's host Node builds from pond/web. npm ci installs the
# panel's packages as package-lock.json pins them, with their checksums, and
# runs none of their install scripts: the build needs none of them (only
# fsevents, for macOS, and the copy of msw that vitest brings have one).
#
# The daemon holds Mongoose, which holds micro-ecc under BSD-2-Clause. The
# panel holds code of packages under MIT and ISC; its build writes their
# licence texts into licenses.txt, which goes into the image with the panel's
# files (pond/web/licences.ts).
#
# pond runs as an account of its own, without a password or a shell, which
# no one logs in with (ADR 0014; post-build.sh writes it): ubusd's access
# list, pond/acl.json, lets it call onu-act and read the state of omcid,
# netifd and dhcp6d, and nothing else.

POND_VERSION = local
POND_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../pond
POND_SITE_METHOD = local
POND_LICENSE = GPL-2.0-only, BSD-2-Clause (micro-ecc in Mongoose), MIT, ISC (packages of the panel)
POND_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt LICENSES/preferred/BSD-2-Clause.txt
POND_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES
POND_DEPENDENCIES = json-c libmnl libubox ubus host-nodejs
POND_NPM = PATH=$(HOST_DIR)/bin:$$PATH npm_config_cache=$(@D)/.npm \
	npm_config_update_notifier=false npm --prefix $(@D)/web

define POND_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
	$(POND_NPM) ci --no-audit --no-fund --ignore-scripts
	$(POND_NPM) run build
endef

define POND_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/pond $(TARGET_DIR)/usr/sbin/pond
	$(INSTALL) -D -m 0644 $(@D)/acl.json $(TARGET_DIR)/usr/share/acl.d/pond.json
	rm -rf $(TARGET_DIR)/usr/share/pond
	mkdir -p $(TARGET_DIR)/usr/share/pond/www
	cp -R $(@D)/web/dist/. $(TARGET_DIR)/usr/share/pond/www/
	chmod -R u=rwX,go=rX $(TARGET_DIR)/usr/share/pond/www
endef

$(eval $(generic-package))

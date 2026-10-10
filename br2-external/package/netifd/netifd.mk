# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# netifd, built from the netifd/ directory of this repository, OpenWrt's
# daemon as forked there. It takes libnl-tiny in statically: the CMake file
# looks the library up with pkg-config unless LIBNL_LIBS names it, and then
# leaves its headers to the compiler's flags.

NETIFD_VERSION = local
NETIFD_SITE = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../netifd
NETIFD_SITE_METHOD = local
NETIFD_LICENSE = GPL-2.0-only
NETIFD_LICENSE_FILES = LICENSES/preferred/GPL-2.0-only.txt
NETIFD_POST_RSYNC_HOOKS += OPEN_DFP_34X_2C_COPY_LICENSES
NETIFD_DEPENDENCIES = json-c libnl-tiny libubox libuci ubus
NETIFD_CONF_OPTS = \
	-DLIBNL_LIBS=$(STAGING_DIR)/usr/lib/libnl-tiny.a \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include/libnl-tiny"

$(eval $(cmake-package))

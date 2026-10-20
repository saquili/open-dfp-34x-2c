# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# procd, OpenWrt's init and service supervisor, at a commit of 2026-10-07,
# with the patches in this directory. procd itself goes into the image, and
# libsetlbf.so, which procd preloads into a service whose output it logs, so
# that the service writes its output a line at a time: procd names it as
# /lib/libsetlbf.so, and nothing loads it otherwise, so it lies in a
# directory of its own, which lib-trim leaves alone, and /lib has a link to
# it. OpenWrt's /sbin/init, which mounts a tmpfs on /dev and runs OpenWrt's
# preinit, udevtrigger, askfirst and the jail are not built. The sources
# carry their licences in their headers, LGPL-2.1 and, for rcS.c, ISC;
# signal.c and rcS.c are named as the licence files.

PROCD_VERSION = 6a22d3964822f6d58fba4c4686e385ddb7e7fcb1
PROCD_SITE = https://git.openwrt.org/project/procd.git
PROCD_SITE_METHOD = git
PROCD_LICENSE = LGPL-2.1-only, ISC (rcS.c)
PROCD_LICENSE_FILES = signal.c rcS.c
PROCD_DEPENDENCIES = json-c libubox ubus
PROCD_MAKE_OPTS = procd setlbf

# procd also logs to udebug, which the image does not carry: neither
# OpenWrt's libudebug nor the daemon that collects the buffers.
PROCD_CONF_OPTS += -DUDEBUG=OFF

define PROCD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/procd $(TARGET_DIR)/sbin/procd
	$(INSTALL) -D -m 0755 $(@D)/libsetlbf.so $(TARGET_DIR)/usr/lib/procd/libsetlbf.so
	ln -sf ../usr/lib/procd/libsetlbf.so $(TARGET_DIR)/lib/libsetlbf.so
endef

$(eval $(cmake-package))

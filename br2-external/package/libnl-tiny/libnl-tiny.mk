# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# libnl-tiny, from OpenWrt. Its one user is netifd, which links the static
# library, so it goes to the staging directory and not into the image. The
# sources carry their licence in their headers; the repository has no licence
# file of its own.

LIBNL_TINY_VERSION = 40493a655d8caa2ccf5206dde1e733abe2920432
LIBNL_TINY_SITE = https://git.openwrt.org/project/libnl-tiny.git
LIBNL_TINY_SITE_METHOD = git
LIBNL_TINY_LICENSE = LGPL-2.1-only
LIBNL_TINY_LICENSE_FILES = nl.c
LIBNL_TINY_INSTALL_STAGING = YES
LIBNL_TINY_INSTALL_TARGET = NO

$(eval $(cmake-package))

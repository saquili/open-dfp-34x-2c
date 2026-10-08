# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

# The licence texts of the repository, for the packages built from its
# directories. A package's source is its own directory, and the texts are at
# the top of the repository, so a package that names them in its
# _LICENSE_FILES takes this hook after its source is copied into the build.
OPEN_DFP_34X_2C_LICENSES = $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/../LICENSES
define OPEN_DFP_34X_2C_COPY_LICENSES
	rm -rf $(@D)/LICENSES
	cp -R $(OPEN_DFP_34X_2C_LICENSES) $(@D)/LICENSES
endef

# The package recipes of this external tree.
include $(sort $(wildcard $(BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH)/package/*/*.mk))

# The kernel headers of the toolchain come from an archive of the committed
# linux/ tree, which tools/kernel-archive writes; OPEN_DFP_34X_2C_KERNEL_ARCHIVE
# is the path it prints. The archive's name holds the id of the tree it was made
# from, so a download cache never serves headers of another kernel.
ifeq ($(BR_BUILDING),y)
ifeq ($(OPEN_DFP_34X_2C_KERNEL_ARCHIVE),)
$(error OPEN_DFP_34X_2C_KERNEL_ARCHIVE is not set: run tools/kernel-archive and pass the path it prints to make)
endif
endif

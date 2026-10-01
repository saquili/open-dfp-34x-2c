# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

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

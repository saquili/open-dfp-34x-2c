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

# Built with the toolchain of a release (configs/external-toolchain.config),
# the image takes the toolchain's sysroot, whose kernel headers are those of
# the commit the toolchain was built from. The programs of this tree are built
# against the uapi headers of this commit's linux/, as the toolchain built from
# source has them, so they are installed over the toolchain's, from the archive
# the kernel is built from, as Buildroot's linux-headers installs them.
ifeq ($(BR2_TOOLCHAIN_EXTERNAL_CUSTOM),y)
OPEN_DFP_34X_2C_UAPI_DIR = $(BUILD_DIR)/open-dfp-34x-2c-uapi
define OPEN_DFP_34X_2C_INSTALL_UAPI
	rm -rf $(OPEN_DFP_34X_2C_UAPI_DIR)
	mkdir -p $(OPEN_DFP_34X_2C_UAPI_DIR)
	tar -xf $(OPEN_DFP_34X_2C_KERNEL_ARCHIVE) --strip-components=1 -C $(OPEN_DFP_34X_2C_UAPI_DIR)
	$(TARGET_MAKE_ENV) $(MAKE) -C $(OPEN_DFP_34X_2C_UAPI_DIR) ARCH=$(KERNEL_ARCH) \
		HOSTCC="$(HOSTCC)" HOSTCFLAGS="$(HOSTCFLAGS)" HOSTCXX="$(HOSTCXX)" \
		INSTALL_HDR_PATH=$(STAGING_DIR)/usr headers_install
	rm -rf $(OPEN_DFP_34X_2C_UAPI_DIR)
endef
TOOLCHAIN_EXTERNAL_CUSTOM_POST_INSTALL_STAGING_HOOKS += OPEN_DFP_34X_2C_INSTALL_UAPI
ifeq ($(BR_BUILDING),y)
ifeq ($(OPEN_DFP_34X_2C_TOOLCHAIN),)
$(error OPEN_DFP_34X_2C_TOOLCHAIN is not set: the top-level Makefile sets it from br2-external/toolchain/fetch)
endif
endif
endif

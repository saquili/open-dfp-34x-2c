# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Top-level entry points for open-dfp-34x-2c.
#
# Until the component a target drives has landed, the target is a stub: it
# prints what it will do and which work item implements it, then exits
# non-zero so that nothing mistakes a stub for a finished build.

.DEFAULT_GOAL := image

.PHONY: image linux omcid sdk legal-info docs check ramboot flash \
        clean distclean help

# The kernel is built out of tree, for MIPS. A Debian host needs the package
# gcc-mips-linux-gnu (the compiler the CI uses), u-boot-tools (mkimage) and
# the usual kernel build tools; name another toolchain with CROSS_COMPILE.
ARCH           = mips
CROSS_COMPILE ?= mips-linux-gnu-
KERNEL_OUT    ?= $(CURDIR)/build/linux
JOBS          ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
KMAKE          = $(MAKE) -C linux O=$(KERNEL_OUT) ARCH=$(ARCH) \
                 CROSS_COMPILE=$(CROSS_COMPILE)

image:
	@echo 'open-dfp-34x-2c: "make" (image) is not implemented yet.'
	@echo '  Planned: build the kernel and root filesystem images for the board.'
	@echo '  Arrives with: the distribution build (WI-14).'
	@exit 1

# The board's configuration and the image the bootloader loads. The kernel is
# $(KERNEL_OUT)/arch/mips/boot/uImage; the root filesystem arrives with the
# distribution build (WI-14).
linux:
	$(KMAKE) rtl9602c_defconfig
	$(KMAKE) -j$(JOBS) uImage

omcid:
	@echo 'open-dfp-34x-2c: "make omcid" is not implemented yet.'
	@echo '  Planned: build the OMCI daemon from omcid/.'
	@echo '  Arrives with: userspace (WI-16).'
	@exit 1

sdk:
	@echo 'open-dfp-34x-2c: "make sdk" is not implemented yet.'
	@echo '  Planned: produce a relocatable cross toolchain for developers.'
	@echo '  Arrives with: the distribution build (WI-14).'
	@exit 1

legal-info:
	@echo 'open-dfp-34x-2c: "make legal-info" is not implemented yet.'
	@echo '  Planned: collect the source and licence bundle a release owes.'
	@echo '  Arrives with: the distribution build (WI-14).'
	@exit 1

docs:
	@echo 'open-dfp-34x-2c: "make docs" is not implemented yet.'
	@echo '  Planned: render the handbook and the kernel documentation.'
	@echo '  Arrives with: the handbook (WI-20).'
	@exit 1

# The provenance gates: their own tests, then the tree and the commits since
# main. The equivalence checks join this target when they land (WI-03).
check:
	tools/provenance/check selftest
	tools/provenance/check all

ramboot:
	@echo 'open-dfp-34x-2c: "make ramboot" is not implemented yet.'
	@echo '  Planned: load a trial kernel on the board over the serial bench.'
	@echo '  Arrives with: bench tooling (WI-15).'
	@exit 1

flash:
	@echo 'open-dfp-34x-2c: "make flash" is not implemented yet.'
	@echo '  Planned: flash an image to the scratch bank, with the bank guard.'
	@echo '  Arrives with: bench tooling (WI-15).'
	@exit 1

clean:
	@echo 'open-dfp-34x-2c: "make clean" is not implemented yet.'
	@echo '  Planned: remove build products.'
	@echo '  Arrives with: the distribution build (WI-14).'
	@exit 1

distclean:
	@echo 'open-dfp-34x-2c: "make distclean" is not implemented yet.'
	@echo '  Planned: remove build products and the download cache.'
	@echo '  Arrives with: the distribution build (WI-14).'
	@exit 1

help:
	@echo 'open-dfp-34x-2c entry points (all but "make linux" and "make check" are stubs today):'
	@echo '  make               build the flashable images (default)'
	@echo '  make linux         build the kernel (uImage)'
	@echo '  make omcid         build the OMCI daemon'
	@echo '  make sdk           build a relocatable cross toolchain'
	@echo '  make legal-info    collect the source-and-licence bundle'
	@echo '  make docs          build the documentation'
	@echo '  make check         run the offline gates'
	@echo '  make ramboot       boot a trial kernel over the bench'
	@echo '  make flash BANK=0  flash an image to the scratch bank'
	@echo '  make clean         remove build products'
	@echo '  make distclean     remove build products and downloads'

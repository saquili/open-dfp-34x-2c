# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Top-level entry points for open-dfp-34x-2c.
#
# Until the component a target drives has landed, the target is a stub: it
# prints what it will do and which work item implements it, then exits
# non-zero so that nothing mistakes a stub for a finished build.

.DEFAULT_GOAL := image

.PHONY: image linux omcid sdk legal-info docs check ramboot flash buildroot \
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

# The pinned build system: the release named in buildroot/VERSION, taken from
# the download directory or fetched, refused unless its size and SHA-256 match
# the pin, extracted into build/buildroot and patched with buildroot/patches. A
# second run changes nothing. BUILDROOT_DL names the directory that holds or
# receives the tarball (default build/dl); ARGS adds options of the script.
buildroot:
	buildroot/fetch $(ARGS)

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

# The offline gates: the provenance gates (their own tests, then the tree and
# the commits since main) and the self-tests of the tools, of the script that
# fetches the build system and of the one that archives the kernel. The
# instruction-set gate's tests skip without a MIPS objdump.
check:
	tools/provenance/check selftest
	tools/provenance/check all
	tools/equiv/tests/run
	python3 -m unittest discover -s tools/bench/tests -t tools
	tools/isa-gate/tests/run
	buildroot/tests/run
	tools/kernel-archive/tests/run

# The bench: tools/bench is a Python package (3.11 or later, standard library
# only) run from tools/. A TOML file of settings is named by BENCH_CONFIG;
# ARGS adds options of the command, for example ARGS=--dry-run. See
# tools/bench/README.rst for the settings and the commands.
BENCH = cd tools && python3 -m bench

# A trial kernel from RAM over TFTP; writes nothing to flash. IMAGE defaults
# to the kernel that "make linux" builds.
ramboot:
	$(BENCH) ramboot $(ARGS) $(if $(IMAGE),"$(abspath $(IMAGE))")

# The scratch bank only: TARGET is kernel (k0) or rootfs (r0). Any BANK but 0
# is refused by the bank guard before any work.
flash:
	@test -n "$(IMAGE)" -a -n "$(TARGET)" || { \
	  echo 'usage: make flash IMAGE=<file> TARGET=kernel|rootfs [BANK=0] [ARGS=...]' >&2; \
	  exit 2; }
	$(BENCH) flash $(if $(filter undefined,$(origin BANK)),,--bank "$(BANK)") \
	  --target "$(TARGET)" $(ARGS) "$(abspath $(IMAGE))"

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
	@echo 'open-dfp-34x-2c entry points (linux, buildroot, check, ramboot and'
	@echo 'flash work; the others are stubs today):'
	@echo '  make               build the flashable images (default)'
	@echo '  make linux         build the kernel (uImage)'
	@echo '  make buildroot     fetch, check and patch the pinned Buildroot'
	@echo '  make omcid         build the OMCI daemon'
	@echo '  make sdk           build a relocatable cross toolchain'
	@echo '  make legal-info    collect the source-and-licence bundle'
	@echo '  make docs          build the documentation'
	@echo '  make check         run the offline gates'
	@echo '  make ramboot       boot a trial kernel from RAM over the bench'
	@echo '  make flash IMAGE=F TARGET=kernel|rootfs [BANK=0]'
	@echo '                     flash an image to the scratch bank'
	@echo '  make clean         remove build products'
	@echo '  make distclean     remove build products and downloads'

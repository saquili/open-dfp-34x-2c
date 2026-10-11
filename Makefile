# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>
#
# Top-level entry points for open-dfp-34x-2c.
#
# "make" builds the two flash images of the board, its update file and the
# package that the shipped firmware's web page installs, and "make sdk" the
# cross toolchain as a tarball. A target that nothing implements yet is a
# stub: it says what to do instead and exits non-zero, so that nothing
# mistakes a stub for a finished build.

.DEFAULT_GOAL := image

.PHONY: image image-check ram-image linux omcid sdk legal-info docs check ramboot flash \
        buildroot clean distclean help

# The kernel is built out of tree, for MIPS. A Debian host needs the package
# gcc-mips-linux-gnu (the compiler the CI uses), u-boot-tools (mkimage) and
# the usual kernel build tools; name another toolchain with CROSS_COMPILE.
ARCH           = mips
CROSS_COMPILE ?= mips-linux-gnu-
KERNEL_OUT    ?= $(CURDIR)/build/linux
JOBS          ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
KMAKE          = $(MAKE) -C linux O=$(KERNEL_OUT) ARCH=$(ARCH) \
                 CROSS_COMPILE=$(CROSS_COMPILE)

# The distribution build: the pinned Buildroot (make buildroot) builds the
# board's images from br2-external/ in IMAGE_OUT, and its toolchain as a
# relocatable tarball in TOOLCHAIN_OUT. Buildroot runs on Linux; on another
# system use the container that docs/handbook/building.rst names. Sources are
# fetched into BR2_DL_DIR and checked against the hashes the build system
# carries; BUILDROOT_DL is where the build system's own tarball is. The kernel
# comes from an archive of the committed linux/ tree (tools/kernel-archive), and
# GIT_CEILING_DIRECTORIES keeps Buildroot out of this repository's history so
# that its version string does not change with the commit.
IMAGE_OUT     ?= $(CURDIR)/build/image
TOOLCHAIN_OUT ?= $(CURDIR)/build/toolchain
BR2_DL_DIR    ?= $(CURDIR)/build/dl
BUILDROOT_DL  ?= $(BR2_DL_DIR)
BR2_ENV        = test "$$(uname -s)" = Linux || { \
	  echo 'open-dfp-34x-2c: Buildroot runs on Linux (docs/handbook/building.rst)' >&2; exit 1; }; \
	br=$$(BUILDROOT_DL="$(BUILDROOT_DL)" buildroot/fetch -q) && \
	export OPEN_DFP_34X_2C_KERNEL_ARCHIVE="$$(tools/kernel-archive/kernel-archive)" \
	  GIT_CEILING_DIRECTORIES="$(CURDIR)" BR2_DL_DIR="$(BR2_DL_DIR)" &&

# The images that must fit their partition for the build to pass: both, since
# bank 0 is split for this system and the images were slimmed to fit it. An
# empty value reports the sizes and fails on neither.
IMAGE_ENFORCE ?= k0 r0

# The build system builds a package once and keeps it. The kernel and its
# headers are built again when the archive of linux/ is another one than last
# time, and the programs of this tree, which it copies from the working tree,
# are copied and built again at every build.
LOCAL_PACKAGES = omcid onu-utils pppoed dhcp6d pond netifd

image:
	$(BR2_ENV) make -C "$$br" O="$(IMAGE_OUT)" BR2_EXTERNAL="$(CURDIR)/br2-external" \
	  odi_dfp_34x_2c3_defconfig && \
	if [ "$$(cat "$(IMAGE_OUT)/.kernel-archive" 2>/dev/null)" != "$$OPEN_DFP_34X_2C_KERNEL_ARCHIVE" ]; then \
	  make -C "$(IMAGE_OUT)" linux-dirclean linux-headers-dirclean && \
	  echo "$$OPEN_DFP_34X_2C_KERNEL_ARCHIVE" > "$(IMAGE_OUT)/.kernel-archive"; \
	fi && \
	rm -rf $(foreach p,$(LOCAL_PACKAGES),"$(IMAGE_OUT)/build/$(p)-local") && \
	make -C "$(IMAGE_OUT)"
	$(MAKE) image-check

# What the build left, judged: the size of each image against its partition, the
# instruction-set gate over every executable of the root filesystem, and the
# digests of the images.
image-check:
	$(if $(IMAGE_ENFORCE),tools/image-budget/image-budget --images "$(IMAGE_OUT)/images" \
	  --enforce $(IMAGE_ENFORCE))
	$(if $(filter-out $(IMAGE_ENFORCE),k0 r0),tools/image-budget/image-budget \
	  --images "$(IMAGE_OUT)/images" $(filter-out $(IMAGE_ENFORCE),k0 r0))
	tools/isa-gate/isa-gate \
	  --objdump "$(IMAGE_OUT)/host/bin/mips-buildroot-linux-uclibc-objdump" "$(IMAGE_OUT)/target"
	cd "$(IMAGE_OUT)/images" && sha256sum -c SHA256SUMS

# A kernel for a RAM boot that carries the image's root filesystem as its
# initramfs, so that a build can run on the board without a write to the
# flash: $(IMAGE_OUT)/images/ram.uImage, for "make ramboot". It takes the
# target tree of "make image", with the tools of diagnosis that post-build.sh
# set aside in $(IMAGE_OUT)/diag and the files of
# OPEN_DFP_34X_2C_LOCAL_OVERLAY laid over a copy that finish-root.sh finishes
# as for the flash's image, and
# builds the board's kernel configuration with the
# image's own cross compiler in RAM_OUT. That configuration leaves out the
# initial RAM disk, which the kernel in the flash does not use, so the RAM
# kernel switches it on. OPEN_DFP_34X_2C_RAM_KCONFIG, options
# for scripts/config such as "--enable NET_DSA_RTL9602C_PLOAM_INJECT", changes
# that configuration for a test image.
RAM_OUT ?= $(CURDIR)/build/ram
RAM_MAKE = $(MAKE) -C linux O="$(RAM_OUT)/kernel" ARCH=$(ARCH) \
	   CROSS_COMPILE="$(IMAGE_OUT)/host/bin/mips-buildroot-linux-uclibc-"

ram-image:
	test -d "$(IMAGE_OUT)/target" || { echo 'open-dfp-34x-2c: run "make image" first' >&2; exit 1; }
	rm -rf "$(RAM_OUT)/root" && mkdir -p "$(RAM_OUT)/root"
	cp -a "$(IMAGE_OUT)/target/." "$(RAM_OUT)/root/"
	if [ -d "$(IMAGE_OUT)/diag" ]; then cp -a "$(IMAGE_OUT)/diag/." "$(RAM_OUT)/root/"; fi
	$(if $(OPEN_DFP_34X_2C_LOCAL_OVERLAY),cp -a "$(OPEN_DFP_34X_2C_LOCAL_OVERLAY)/." "$(RAM_OUT)/root/")
	rm -f "$(RAM_OUT)/root/THIS_IS_NOT_YOUR_ROOT_FILESYSTEM"
	sh br2-external/board/dfp-34x-2c3/finish-root.sh "$(RAM_OUT)/root"
	$(RAM_MAKE) rtl9602c_defconfig
	linux/scripts/config --file "$(RAM_OUT)/kernel/.config" --enable BLK_DEV_INITRD \
	  --set-str INITRAMFS_SOURCE "$(RAM_OUT)/root $(CURDIR)/br2-external/board/dfp-34x-2c3/initramfs.list" \
	  --set-val INITRAMFS_ROOT_UID 0 --set-val INITRAMFS_ROOT_GID 0
	$(if $(OPEN_DFP_34X_2C_RAM_KCONFIG),linux/scripts/config --file "$(RAM_OUT)/kernel/.config" $(OPEN_DFP_34X_2C_RAM_KCONFIG))
	$(RAM_MAKE) olddefconfig
	$(RAM_MAKE) -j$(JOBS) uImage
	cp "$(RAM_OUT)/kernel/arch/mips/boot/uImage" "$(IMAGE_OUT)/images/ram.uImage"
	ls -l "$(IMAGE_OUT)/images/ram.uImage"

# The kernel alone, with the Debian cross compiler, for development: the board's
# configuration and the image the bootloader loads, which is
# $(KERNEL_OUT)/arch/mips/boot/uImage. "make" builds the kernel with the
# distribution's own toolchain, together with the root filesystem.
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
	@echo '  "make" builds the OMCI daemon into the image; "make -C omcid" builds'
	@echo '  it for this host and "make -C omcid check" runs its tests (omcid/README.rst).'
	@exit 1

# The cross toolchain of configs/toolchain_defconfig, with static and shared
# libraries, packed by the build system as a tarball that can be unpacked
# anywhere: TOOLCHAIN_OUT/images/*.tar.gz (docs/handbook/toolchain.rst).
sdk:
	$(BR2_ENV) make -C "$$br" O="$(TOOLCHAIN_OUT)" BR2_EXTERNAL="$(CURDIR)/br2-external" \
	  toolchain_defconfig && make sdk -C "$(TOOLCHAIN_OUT)" && \
	  ls -l "$(TOOLCHAIN_OUT)"/images/*.tar.gz

# The licence information a release owes, which the build system collects for
# the board's configuration into IMAGE_OUT/legal-info: the licence of every
# package, their licence texts and the sources it may hand on. The licences of
# the panel's packages are written by the panel's build (pond/web/licences.ts),
# so they are taken from a built image: run make first.
legal-info:
	$(BR2_ENV) make -C "$$br" O="$(IMAGE_OUT)" BR2_EXTERNAL="$(CURDIR)/br2-external" \
	  odi_dfp_34x_2c3_defconfig && make -C "$(IMAGE_OUT)" legal-info
	@panel="$(IMAGE_OUT)/target/usr/share/pond/www/licenses.txt.gz"; \
	if [ ! -f "$$panel" ]; then \
	  echo "open-dfp-34x-2c: no $$panel: the panel's licences come from its build; run make first" >&2; \
	  exit 1; \
	fi; \
	gzip -dc "$$panel" > "$(IMAGE_OUT)/legal-info/pond-panel-licenses.txt" && \
	echo "open-dfp-34x-2c: the licence information is in $(IMAGE_OUT)/legal-info"

docs:
	@echo 'open-dfp-34x-2c: "make docs" is not implemented yet.'
	@echo '  The pages are reStructuredText, read as they are (docs/README.rst);'
	@echo '  "make -C linux htmldocs" renders the kernel documentation.'
	@exit 1

# The offline gates: the provenance gates (their own tests, then the tree and
# the commits since main) and the self-tests of the tools, of the script that
# fetches the build system, of the one that archives the kernel, of the size
# check of the images, of the cut of the shared libraries and of the scripts and
# lists of the distribution. The instruction-set gate's tests skip without a
# MIPS objdump, the cut's end-to-end tests without a MIPS cross compiler, and
# the tests of the image script without mksquashfs.
check:
	tools/provenance/check selftest
	tools/provenance/check all
	tools/equiv/tests/run
	python3 -m unittest discover -s tools/bench/tests -t tools
	tools/isa-gate/tests/run
	tools/lib-trim/tests/run
	buildroot/tests/run
	tools/kernel-archive/tests/run
	tools/image-budget/tests/run
	br2-external/tests/run

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
	@echo '  Every build product is under build/; remove the directory of one build'
	@echo '  (build/image, build/ram, build/linux, build/toolchain) to start it again.'
	@exit 1

distclean:
	@echo 'open-dfp-34x-2c: "make distclean" is not implemented yet.'
	@echo '  Every build product and the download cache (build/dl) are under build/.'
	@exit 1

help:
	@echo 'open-dfp-34x-2c entry points (make omcid, make docs, make clean and'
	@echo 'make distclean are stubs today; the others work):'
	@echo '  make               build k0.uImage, r0.squashfs, update.bin and'
	@echo '                     web-install.tar in build/image/images (default;'
	@echo '                     needs Linux), then check them as make image-check'
	@echo '  make image-check   check a built image: sizes, instruction set, digests'
	@echo '  make ram-image     after make: the kernel with the image as its initramfs,'
	@echo '                     build/image/images/ram.uImage, for make ramboot'
	@echo '  make linux         build the kernel (uImage) with the Debian cross compiler'
	@echo '  make buildroot     fetch, check and patch the pinned Buildroot'
	@echo '  make omcid         build the OMCI daemon'
	@echo '  make sdk           build a relocatable cross toolchain tarball'
	@echo '  make legal-info    collect the licence information of a built image'
	@echo '                     in build/image/legal-info (needs Linux)'
	@echo '  make docs          build the documentation'
	@echo '  make check         run the offline gates'
	@echo '  make ramboot       boot a trial kernel from RAM over the bench'
	@echo '  make flash IMAGE=F TARGET=kernel|rootfs [BANK=0]'
	@echo '                     flash an image to the scratch bank'
	@echo '  make clean         remove build products'
	@echo '  make distclean     remove build products and downloads'

.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============
br2-external
============

The distribution: a Buildroot external tree (see ADR 0005).

This is where the product is assembled from source -- the board definition, the
target configuration, the C-library and BusyBox configurations, the package
recipes (including the OMCI daemon), and the post-image script that lays out the
two partition images the bootloader loads. The kernel comes from ``linux/`` and
the pinned build system from ``buildroot/``.

So far the tree builds the cross toolchain, and nothing else: the root
filesystem, the board's configuration and the OMCI daemon package arrive with
the rest of **WI-14**.

What is here
============

``external.desc``, ``Config.in``, ``external.mk``
    The three files Buildroot requires of an external tree. The tree is named
    ``OPEN_DFP_34X_2C``, so Buildroot sets ``BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH``
    to this directory. ``Config.in`` has no entry yet; ``external.mk`` includes
    the package recipes under ``package/`` (there are none) and refuses to run a
    build without the kernel archive described below.
``configs/toolchain_defconfig``
    The configuration of the toolchain, and of nothing but the toolchain.
``board/dfp-34x-2c3/uclibc.fragment``
    The uClibc-ng settings that the userspace of the board needs, which the
    defconfig names.

The toolchain
=============

``configs/toolchain_defconfig`` builds Buildroot's internal toolchain from
source:

======================= ======================================================
MIPS I                  big endian, soft-float; the architecture variant that
                        the carried patch adds (``buildroot/README.rst``)
gcc 14.4.0              C only; no C++, no Fortran, one library set (no
                        multilib); configured with ``--with-arch=mips1``
binutils 2.44
uClibc-ng 1.0.59        static and shared libraries, no threads, the stack
                        protector on (see below)
Linux 6.18.48 headers   those of ``linux/`` (see below)
======================= ======================================================

``BR2_REPRODUCIBLE`` is on. The root filesystem is off: no BusyBox, no init
system, no image. Building it takes about four minutes on 18 cores with the
sources in the download cache.

Building it
-----------

On Debian 13 the build needs ``ca-certificates``, ``git``, ``python3``,
``patch``, ``xz-utils``, ``make``, ``gcc``, ``g++``, ``libc6-dev``,
``binutils``, ``perl``, ``bzip2``, ``gzip``, ``cpio``, ``unzip``, ``rsync``,
``file``, ``bc``, ``findutils``, ``gawk``, ``wget``, ``sed``, ``diffutils``,
``tar`` and ``debianutils``. From the top of the repository::

    make buildroot
    export OPEN_DFP_34X_2C_KERNEL_ARCHIVE=$(tools/kernel-archive/kernel-archive)
    export GIT_CEILING_DIRECTORIES=$PWD
    export BR2_DL_DIR=$PWD/build/dl
    br=build/buildroot/buildroot-2025.02.18
    make -C $br O=$PWD/build/toolchain BR2_EXTERNAL=$PWD/br2-external \
        toolchain_defconfig
    make sdk -C build/toolchain

``make sdk`` leaves the toolchain as a relocatable tarball,
``build/toolchain/images/*.tar.gz`` (its name begins with the target,
``mips-buildroot-linux-uclibc``): unpack it anywhere and run the script
``./relocate-*.sh`` in the directory it made. The toolchain itself, before it is
packed, is ``build/toolchain/host``. What each line is for:

* ``tools/kernel-archive/kernel-archive`` makes the archive of the kernel that the
  headers come from and prints its path (below).
* ``GIT_CEILING_DIRECTORIES`` keeps Buildroot out of this repository's history.
  Buildroot asks git for a version string, and the tree it was unpacked into,
  ``build/buildroot``, lies inside this repository. Without the variable the
  version, which the compiler reports, would be made from the commit that
  happens to be checked out (``-g`` and its abbreviated id, or the description
  of the nearest tag when there is one, and ``-dirty`` after an uncommitted
  change), so the toolchain would differ from one commit to the next. With it
  the version is ``2025.02.18`` whatever the checkout (the output of
  ``make -C build/toolchain printvars VARS=BR2_VERSION_FULL`` shows it).
* ``BR2_DL_DIR`` is the download cache. Buildroot fetches the sources of the
  toolchain itself and checks each against the hash it carries: gcc, binutils,
  uClibc-ng, gmp, mpfr, mpc, and the helper tools that its build uses (autoconf,
  automake, libtool, m4, patchelf). ``make -C build/toolchain show-info`` lists
  the packages and their sources.

What the build makes can be checked by compiling a file with no options and
looking at the compiler and at the object::

    t=build/toolchain/host/bin/mips-buildroot-linux-uclibc
    $t-gcc -v 2>&1 | grep -o -e '--with-[a-z0-9-]*=[a-z0-9]*' -e '--without-[a-z]*'
    echo 'int f(int a, int b) { return a / b; }' > /tmp/f.c
    $t-gcc -O2 -c /tmp/f.c -o /tmp/f.o
    $t-readelf -h /tmp/f.o | grep -e Flags -e Data
    $t-readelf -A /tmp/f.o | grep -e ISA -e 'FP ABI'

The compiler is configured with ``--with-arch=mips1``, ``--with-abi=32``,
``--with-float=soft``, ``--with-divide=breaks``, ``--without-llsc`` and
``--without-synci``; the object has the flags ``mips1``, ``o32``, big endian, and
the ABI flags ``ISA: MIPS1`` and ``FP ABI: Soft float``.

Kernel headers
--------------

The C library is built against the uapi headers of this repository's own kernel,
not of a release that Buildroot lists. The newest it offers is Linux 6.12, which
has none of the interfaces our series adds, ``include/uapi/linux/pon.h`` among
them, and the headers of 6.12 would also differ from those of the kernel that the
board runs. So the defconfig names a custom archive
(``BR2_KERNEL_HEADERS_CUSTOM_TARBALL``) that ``tools/kernel-archive`` makes from
the ``linux/`` tree of the checked-out commit, and declares the series as "6.12.x
or later", the newest series that the release's menu knows. The location is
``file://$(OPEN_DFP_34X_2C_KERNEL_ARCHIVE)``; ``external.mk`` stops a build that
does not set the variable. The sysroot then holds ``usr/include/linux/pon.h`` and
both files of the pairs that differ only in case, and
``usr/include/linux/version.h`` says 6.18.48.

Buildroot does not check a hash for a custom archive. The archive's name carries
the id of the ``linux/`` tree it was made from, so that the download cache,
which serves a file it has under the same name without looking at it, cannot hand
out the headers of another kernel.

No threads
----------

The toolchain has no thread library (``BR2_PTHREADS_NONE``), and the patch does
not offer one. uClibc-ng's thread libraries, and its atomic header, are built on
the load-linked and store-conditional pair: on this core they assemble and then
never complete. A program without threads has nothing that must be atomic, and
gcc is configured with ``--disable-threads``, ``--disable-tls`` and
``--disable-libatomic``.

The C library's configuration
-----------------------------

``board/dfp-34x-2c3/uclibc.fragment`` is merged over Buildroot's own uClibc-ng
defaults. The architecture, the ABI, the endianness, soft-float, the shared
libraries, the absence of threads and the removal of the MIPS prefetch routines
are set by the toolchain options and the patch, and the fragment does not repeat
them. It names what the userspace of the board needs the C library to offer: the
C99 maths functions, ``ftw``, IPv6 in the library, and ``fopencookie``. The stack
protector is on (``BR2_TOOLCHAIN_BUILDROOT_USE_SSP``), which builds the C library
itself with it as well.

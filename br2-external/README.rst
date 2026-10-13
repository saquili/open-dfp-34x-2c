.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

============
br2-external
============

The distribution: a Buildroot external tree (see ADR 0005).

This is where the product is assembled from source -- the board definition, the
target configuration, the C-library and BusyBox configurations, the package
recipes, and the post-image script that lays out the two partition images the
bootloader loads. The kernel comes from ``linux/`` and the pinned build system
from ``buildroot/``.

The tree builds the cross toolchain, and the images of the DFP-34X-2C3: the
kernel; a root filesystem of procd, ubus, netifd, BusyBox, dropbear and the
programs of this tree, with the boot script; the update file of the stick's
``onu-update``; and the package that the shipped firmware's web page installs
(see "The board's image").

What is here
============

``external.desc``, ``Config.in``, ``external.mk``
    The three files Buildroot requires of an external tree. The tree is named
    ``OPEN_DFP_34X_2C``, so Buildroot sets ``BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH``
    to this directory. ``Config.in`` names the packages of ``package/``;
    ``external.mk`` includes their recipes and refuses to run a build without
    the kernel archive described below.
``package/``
    The recipes of the packages Buildroot does not carry: ``omcid``,
    ``onu-utils``, ``pppoed``, ``dhcp6d``, ``pond`` and ``netifd``, each built
    from its directory of this repository (``SITE_METHOD = local``); and
    ``procd`` and ``libnl-tiny`` of OpenWrt, fetched by hash, with ``procd``'s
    patches (ADR 0013). libubox, ubus, json-c and libuci are Buildroot's own
    packages, which ``buildroot/patches`` builds for this core.
``configs/toolchain_defconfig``
    The configuration of the toolchain, and of nothing but the toolchain.
``configs/odi_dfp_34x_2c3_defconfig``
    The board: the same toolchain with shared libraries only, the kernel, the
    root filesystem and the two images (see "The board's image").
``board/dfp-34x-2c3/uclibc.fragment``
    The uClibc-ng settings that the userspace of the board needs, which both
    defconfigs name.
``board/dfp-34x-2c3/busybox.config``
    The BusyBox configuration of the image: a complete one, with the applets that
    the boot script and a shell session use.
``board/dfp-34x-2c3/overlay/``
    The files laid over the root filesystem: the first program ``sbin/init``,
    which starts procd, procd's inittab, the boot script ``etc/init.d/rcS``
    (``etc/rc.d/S10boot``), ``etc/onu.conf``, which says what each key of the
    settings means and sets none, the shell profile, the name of the image,
    and the ``var`` directory that a temporary file system is mounted on.
``board/dfp-34x-2c3/post-build.sh``
    The script that the build system runs on the target tree: it takes out
    iproute2, the command-line tools of the network layer (``uci``, ``jshn``)
    and the tools of diagnosis, and sets the latter aside in ``diag/`` of the
    build's directory for the RAM image; it cuts the shared libraries with
    ``tools/lib-trim`` and writes the build's commit into
    ``/etc/os-release``.
``board/dfp-34x-2c3/finish-root.sh``
    What a copy of the target tree needs before it becomes a root filesystem,
    the flash's or the RAM image's: its accounts among them.
``board/dfp-34x-2c3/post-image.sh``
    The script that leaves ``k0.uImage``, ``r0.squashfs``, ``update.bin``,
    ``web-install.tar`` and ``SHA256SUMS`` in the images directory, with
    ``mkupdate.py``, which makes the update file, and ``mkweb.py``, which
    makes the package of the shipped firmware's web page around ``fwu.sh``,
    the script that installs it.
``board/dfp-34x-2c3/initramfs.list``
    What the initramfs of a RAM image holds beyond the target tree: the
    console's device and ``/init``.
``tests/run``
    The unit tests of the tree (``tests/image`` and ``tests/toolchain``).
``tests/image/``
    The tests of the post-image script, the boot script and the defconfig,
    and of the shell scripts of the image: ``test_scripts.py`` reads them
    (``shellwords.py`` and ``imagescripts.py`` do the reading) and holds every
    command that one runs against what the image has, which the tests of
    ``onu-utils``, on a build machine with a GNU userland, cannot see. And the
    checks that run on a built image: ``check-applets`` asks the tree the same
    question, runs its programs under ``qemu-mips`` and ``tree-digest`` lists
    its files with their digests.
``tests/toolchain/``
    The programs and scripts that check a toolchain built from the toolchain
    defconfig (see "Checks on the toolchain"), and the list of the findings of the
    instruction-set gate in the toolchain's own libraries that are known.

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

``BR2_REPRODUCIBLE`` is on, and binutils is configured with
``--enable-deterministic-archives``, so that ``ar`` and ``ranlib`` put no time or
owner into an archive. This defconfig builds no root filesystem: no BusyBox, no
init system, no image. Building it takes about four minutes on 18 cores with the
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

The board's image
=================

``configs/odi_dfp_34x_2c3_defconfig`` builds the kernel and the root filesystem
of the stick and leaves the two flash images in ``images/`` of the output
directory. ``make`` in the top directory does it; ``docs/handbook/building.rst``
has the commands, the sizes and how to put the images on the board. In short:

* The toolchain is the one above with shared libraries only
  (``BR2_SHARED_LIBS``): the programs of the image share one C library, and are
  not position-independent (``BR2_PIC_PIE`` off, so RELRO is partial). The
  post-build script cuts the shared libraries to what the programs use with
  ``tools/lib-trim``, which removes ``libgcc_s`` (nothing loads it), links the C
  library again with only the functions the programs import, and fails the
  build when a program would miss a library or a symbol (ADR 0011).
* The kernel is the tree of ``linux/``, taken from the archive that serves the
  headers, configured by ``rtl9602c_defconfig`` and built with this toolchain.
  The image is the LZMA-compressed legacy U-Boot image the boot loader takes.
* The root filesystem is BusyBox 1.37.0 (``board/dfp-34x-2c3/busybox.config``),
  dropbear 2026.93, and the files of ``board/dfp-34x-2c3/overlay``. The build
  system installs no init scripts (``BR2_INIT_NONE``): the overlay's
  ``sbin/init`` starts procd, which reads the overlay's ``etc/inittab`` and
  runs its boot script, ``etc/init.d/rcS`` (ADR 0013).
  The files that belong to one bench and not to the repository, an ssh key or
  other management addresses, are in a directory named by
  ``OPEN_DFP_34X_2C_LOCAL_OVERLAY``; the post-image script lays it over a copy of
  the target tree, so the target directory keeps none of them.
* ``board/dfp-34x-2c3/post-image.sh`` makes ``r0.squashfs`` from the target
  directory with the host ``mksquashfs`` (xz, every file root-owned and
  carrying the build's time, no extended attributes), copies the kernel's
  ``uImage`` to ``k0.uImage``, makes ``update.bin`` and ``web-install.tar``
  from the two, and writes ``SHA256SUMS``.
* The build is reproducible: ``BR2_REPRODUCIBLE`` fixes ``SOURCE_DATE_EPOCH``,
  which the kernel build, ``mkimage`` and the post-image script use for every
  time they write; the kernel builds as ``buildroot@buildroot``. As for the
  toolchain, ``GIT_CEILING_DIRECTORIES`` keeps the version of the build system
  from following the commit.

What the build judges is in the top-level ``Makefile``: the images against their
partitions (``tools/image-budget``), the instruction-set gate over the whole
target tree, and the digests. ``tests/image/check-applets`` checks that every
command the shell scripts of the image run is in the image, and runs the image's
programs under ``qemu-mips``, with the image's own loader and cut C library, and ``tests/image/tree-digest`` lists a
target tree for the comparison of two builds; the ``image`` workflow does both
(``.github/workflows/README.rst``).

Checks on the toolchain
=======================

The checks run on the toolchain in ``build/toolchain/host`` or on an unpacked and
relocated tarball. They need ``binutils-mips-linux-gnu``, for the ``objdump`` of
the instruction-set gate, ``qemu-user`` and ``gcc-mips-linux-gnu``.

The instruction-set gate over the toolchain's own objects
----------------------------------------------------------

``tools/isa-gate`` is run over every object the toolchain holds for the target:
the start files, the C library as an archive and as a shared library, the
dynamic loader, libgcc in both forms and the helper programs of the C library.
``tests/toolchain/target-objects`` lists them by reading the ELF headers, which
leaves out the programs that run on the build machine::

    br2-external/tests/toolchain/target-objects build/toolchain/host \
        | xargs tools/isa-gate/isa-gate --archives

For the toolchain this defconfig builds, the list holds 22 files and the gate
scans 3109 objects, 366584 instructions. It reports 34 findings, all in two
functions of the libraries:

``__speculation_barrier`` (libgcc.a and libgcc_s.so.1, 10 findings in each file)
    A ``sync``, written as a data word because MIPS I has none, eight ``ssnop``
    and a ``jr.hb`` word. gcc 14 builds it for every MIPS target
    (``LIB1ASMFUNCS`` in ``libgcc/config/mips/t-mips``). A program reaches it
    through ``__builtin_speculation_safe_value`` only.
``_test_and_set`` (libc.a and libuClibc.so, 2 findings in each file)
    A load-linked and store-conditional loop, the function behind
    ``<sys/tas.h>``; uClibc-ng builds it for every MIPS target
    (``libc/sysdeps/linux/mips/Makefile.arch``).

A third, ``__cxa_finalize`` of uClibc-ng, ran a compare-and-exchange for each
entry of the exit-handler table; the carried patch of the exit handlers
(``buildroot/README.rst``) claims the entry with a plain test and store when
there are no threads, and it is no longer a finding.

No option of gcc or of uClibc-ng leaves the two functions out. A program that
links the C library statically takes only the objects it refers to, so a static
program does not contain them; the test programs below, static and dynamic, pass
the gate, and none of them executes a load-linked, store-conditional or sync
instruction. The image's programs share the C library, which the build cuts to
the functions they import: no program imports ``_test_and_set``, so the image's
library does not hold it, and the image does not ship ``libgcc_s``, which nothing
loads (ADR 0011). The gate over the target tree holds the image to that.

``tests/toolchain/known-findings.txt`` lists the findings, for each file, archive
member and function, with the instructions in the order the gate reports them.
``compare-findings`` holds the gate's output to the list: the toolchain job
passes when the findings are exactly the list, and fails when one is new, gone or
different. On the toolchain this defconfig builds the 34 findings are the 5
groups of the list. When the toolchain changes on purpose,
``compare-findings --print-known`` writes the new list from the gate's output.

The test programs
-----------------

``tests/toolchain/t-*.c`` are 14 small programs: formatted input and output, the
allocator, the string and memory functions, ``qsort``, calendar and clock
functions, ``poll`` and ``select``, sockets that need no network, signals,
64-bit division, shifts by a run-time count, unaligned loads and stores through
``memcpy``, exit handlers, soft-float arithmetic and the maths library, and
integer division by zero (which the compiler checks with ``break``). Each
checks its own results and prints ``ok <name>``; the values that some print are
checked against exact arithmetic done outside the programs. ``expected.txt`` holds
what each prints. ::

    br2-external/tests/toolchain/run build/toolchain/host

builds every program with ``-O2`` and ``-Wall -Wextra -Werror``, static and
dynamic, into ``build/toolchain-tests``; runs the gate over the 28 binaries; runs
each of 13 of the programs under ``qemu-mips`` (the dynamic ones with the
toolchain's sysroot) and compares its output with ``expected.txt``; and searches
the instructions that ``qemu-mips`` translated for ``ll``, ``sc`` and ``sync``.
``t-divzero`` is built and checked by the gate, and not run under ``qemu-mips``,
which raises SIGTRAP for the ``break`` where the kernel of the board raises
SIGFPE.

To run them on the board, copy the ``static`` directory, and ``check-output`` and
``expected.txt`` of ``br2-external/tests/toolchain``, to it, and run::

    sh check-output static

The dynamic programs ask for ``/lib/ld-uClibc.so.0`` as their loader and need
the C library, ``libc.so.0``, beside it: the sysroot's ``lib`` has both and the
links between them (``readelf -l`` and ``readelf -d`` of a program show the
names). In a directory that is not the board's root, ``check-output`` can run
them through ``chroot``::

    mkdir -p /tmp/root/lib /tmp/root/t
    cp -a lib/* /tmp/root/lib/
    cp dynamic/t-* /tmp/root/t/
    ROOT=/tmp/root RUN_PREFIX='chroot /tmp/root' sh check-output /tmp/root/t

``check-output`` needs a POSIX shell and ``sed``; it prints ``pass`` or ``FAIL``
for each program and exits non-zero if one failed. On a build machine,
``RUN_PREFIX='qemu-mips'`` runs the static programs and
``RUN_PREFIX='qemu-mips -L <sysroot>'`` the dynamic ones; ``t-divzero`` fails
under ``qemu-mips``, as above.

The negative control
--------------------

``tests/toolchain/negative-control`` compiles four functions with the Debian cross
compiler for MIPS, whose default is MIPS32 release 2, and requires the gate to
refuse the object: for its architecture flags, for the load-linked and
store-conditional pair (``lwc0`` and ``swc0``), and for the instructions that a
listing as MIPS I leaves as data words (``clz``, ``mul``, ``wsbh`` and
``sync``). It shows that the gate can fail on code that this core cannot run.

Two builds
----------

``tests/toolchain/compare-trees`` compares the host directories of two builds
made in different directories. A toolchain records the directory it was built in
in some of its files: in the paths that its programs search, and in debugging
information. The two builds are therefore made in directories whose names differ
and have the same length (``build/toolchain-a`` and ``build/toolchain-b``), and
the files of the first are compared with the files of the second after the one
name is replaced by the other. Two values that a file holds are computed from
the rest of it and change with it, so they are set aside: the build ID of an ELF
file, and the 16-byte ``executable_checksum`` that the compiler proper
(``cc1``) records for its precompiled-header and plug-in checks. The trees have
to hold the same files, links and modes, and every file has to be identical after
that::

    br2-external/tests/toolchain/compare-trees build/toolchain-a/host \
        build/toolchain-b/host toolchain-a toolchain-b

It prints how many files were identical as they were, how many after the
replacement and the setting aside, and the files that still differ; its exit
status is non-zero if there is one. The tarballs that ``make sdk`` writes are not
compared: a tarball holds the modification times of the files as they were
installed.

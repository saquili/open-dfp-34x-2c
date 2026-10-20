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

The tree builds the cross toolchain, which the repository releases on its own
(see "The toolchain of a release"), and, with the toolchain of such a release,
the images of the DFP-34X-2C3: the kernel; a root filesystem of procd, ubus,
netifd, BusyBox, dropbear and the programs of this tree, with the boot script;
the update file of the stick's ``onu-update``; and the package that the
shipped firmware's web page installs (see "The board's image").

What is here
============

``external.desc``, ``Config.in``, ``external.mk``
    The three files Buildroot requires of an external tree. The tree is named
    ``OPEN_DFP_34X_2C``, so Buildroot sets ``BR2_EXTERNAL_OPEN_DFP_34X_2C_PATH``
    to this directory. ``Config.in`` names the packages of ``package/``;
    ``external.mk`` includes their recipes, refuses to run a build without
    the kernel archive described below, and installs the uapi headers of the
    checked-out kernel over those of a toolchain of a release.
``package/``
    The recipes of the packages Buildroot does not carry: ``omcid``,
    ``onu-utils``, ``pppoed``, ``dhcp6d``, ``pond`` and ``netifd``, each built
    from its directory of this repository (``SITE_METHOD = local``); and
    ``procd`` and ``libnl-tiny`` of OpenWrt, fetched by hash, with ``procd``'s
    patches (ADR 0010). libubox, ubus, json-c and libuci are Buildroot's own
    packages, which ``buildroot/patches`` builds for this core.
``configs/toolchain_defconfig``
    The configuration of the toolchain, and of nothing but the toolchain.
``configs/odi_dfp_34x_2c3_defconfig``
    The board: the same toolchain with shared libraries only, the kernel, the
    root filesystem and the two images (see "The board's image").
``configs/external-toolchain.config``
    The fragment that the top-level ``Makefile`` merges over the board's
    defconfig when the images are built with the toolchain of a release.
``toolchain/``
    ``PIN``, which names the release of the toolchain that the images are
    built with; ``fetch``, which fetches its files, checks them against the
    pin and unpacks the toolchain; and ``keep-uclibc``, which puts the pieces
    of the C library's link into a toolchain before ``make sdk`` packs it
    (see "The toolchain of a release").
``board/dfp-34x-2c3/uclibc.fragment``
    The uClibc-ng settings that the userspace of the board needs, which both
    defconfigs name.
``board/dfp-34x-2c3/busybox.config``
    The BusyBox configuration of the image: a complete one, with the applets
    that the boot script and a shell session use.
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
    defconfig (see "Checks on the toolchain"), the list of the findings of
    the instruction-set gate in the toolchain's own libraries that are known,
    and the tests of ``toolchain/fetch``.

The toolchain
=============

``configs/toolchain_defconfig`` builds Buildroot's internal toolchain from
source:

======================= ======================================================
MIPS I                  big endian, soft-float, with ``ll``, ``sc`` and
                        ``sync``; the architecture variant that the carried
                        patch adds (``buildroot/README.rst``)
gcc 14.4.0              C only; no C++, no Fortran, one library set (no
                        multilib); configured with ``--with-arch=mips1`` and
                        ``--with-llsc``
binutils 2.44
uClibc-ng 1.0.59        static and shared libraries, no threads, the stack
                        protector on (see below)
Linux 6.18.57 headers   those of ``linux/`` (see below)
======================= ======================================================

``BR2_REPRODUCIBLE`` is on, and binutils is configured with
``--enable-deterministic-archives``, so that ``ar`` and ``ranlib`` put no time
or owner into an archive. This defconfig builds no root filesystem: no
BusyBox, no init system, no image. It builds the C library and libgcc with the
flags of the board's programs (``BR2_OPTIMIZE_S``, ``BR2_PIC_PIE`` off,
``BR2_RELRO_PARTIAL``) and installs no ``ldd`` or ``getconf`` for the target,
so that an image built with this toolchain takes from its sysroot what the
board's own build of the toolchain would give it.
``docs/handbook/building.rst`` gives the time a build takes.

Building it
-----------

On Debian 13 the build needs ``ca-certificates``, ``git``, ``python3``,
``patch``, ``xz-utils``, ``make``, ``gcc``, ``g++``, ``libc6-dev``,
``binutils``, ``perl``, ``bzip2``, ``gzip``, ``cpio``, ``unzip``, ``rsync``,
``file``, ``bc``, ``findutils``, ``gawk``, ``wget``, ``sed``, ``diffutils``,
``tar`` and ``debianutils``. From the top of the repository::

    make sdk

builds it in ``build/toolchain`` (``TOOLCHAIN_OUT`` names another directory)
and packs it. What it runs is, in effect::

    make buildroot
    export OPEN_DFP_34X_2C_KERNEL_ARCHIVE=$(tools/kernel-archive/kernel-archive)
    export GIT_CEILING_DIRECTORIES=$PWD
    export BR2_DL_DIR=$PWD/build/dl
    br=build/buildroot/buildroot-2025.02.18
    make -C $br O=$PWD/build/toolchain BR2_EXTERNAL=$PWD/br2-external \
        toolchain_defconfig
    make -C build/toolchain
    br2-external/toolchain/keep-uclibc build/toolchain

and last the build system's own ``make sdk`` in ``build/toolchain``, with the
name of the tarball that the top-level ``Makefile`` gives it
(``TOOLCHAIN_NAME``). That leaves the toolchain as a relocatable tarball,
``build/toolchain/images/open-dfp-34x-2c-toolchain.tar.gz``, whose one
directory has the same name: unpack it anywhere and run the script
``./relocate-*.sh`` in that directory. The toolchain itself, before it is
packed, is ``build/toolchain/host``. What each line is for:

* ``tools/kernel-archive/kernel-archive`` makes the archive of the kernel that
  the headers come from and prints its path (below).
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
* ``keep-uclibc`` puts the pieces of the C library's link into the toolchain
  before ``make sdk`` packs it (see "The toolchain of a release").

What the build makes can be checked by compiling a file with no options and
looking at the compiler and at the object::

    t=build/toolchain/host/bin/mips-buildroot-linux-uclibc
    $t-gcc -v 2>&1 | grep -o -e '--with-[a-z0-9-]*=[a-z0-9]*' -e '--with-llsc' \
        -e '--without-[a-z]*'
    echo 'int f(int a, int b) { return a / b; }' > /tmp/f.c
    $t-gcc -O2 -c /tmp/f.c -o /tmp/f.o
    $t-readelf -h /tmp/f.o | grep -e Flags -e Data
    $t-readelf -A /tmp/f.o | grep -e ISA -e 'FP ABI'

The compiler is configured with ``--with-arch=mips1``, ``--with-abi=32``,
``--with-float=soft``, ``--with-divide=breaks``, ``--with-llsc`` and
``--without-synci``; the object has the flags ``mips1``, ``o32``, big endian,
and the ABI flags ``ISA: MIPS1`` and ``FP ABI: Soft float``.

Kernel headers
--------------

The C library is built against the uapi headers of this repository's own kernel,
not of a release that Buildroot lists. The newest it offers is Linux 6.12, which
has none of the interfaces our series adds, ``include/uapi/linux/pon.h`` among
them, and the headers of 6.12 would also differ from those of the kernel that
the board runs. So the defconfig names a custom archive
(``BR2_KERNEL_HEADERS_CUSTOM_TARBALL``) that ``tools/kernel-archive`` makes from
the ``linux/`` tree of the checked-out commit, and declares the series as
"6.12.x or later", the newest series that the release's menu knows. The location
is ``file://$(OPEN_DFP_34X_2C_KERNEL_ARCHIVE)``; ``external.mk`` stops a build
that does not set the variable. The sysroot then holds
``usr/include/linux/pon.h`` and both files of the pairs that differ only in
case, and ``usr/include/linux/version.h`` says 6.18.57.

Buildroot does not check a hash for a custom archive. The archive's name carries
the id of the ``linux/`` tree it was made from, so that the download cache,
which serves a file it has under the same name without looking at it, cannot
hand out the headers of another kernel.

No threads
----------

The toolchain has no thread library (``BR2_PTHREADS_NONE``), and the patch
offers neither of uClibc-ng's. NPTL finds each thread's storage through the
thread pointer, which the MIPS ABI reads with ``rdhwr $3, $29``; on this core
that instruction faults as an access to memory instead of trapping, so the
kernel cannot emulate it (M, ``linux/Documentation/arch/mips/rtl9602c.rst``,
"Instructions beyond MIPS I"). linuxthreads is not kept up. gcc is configured
with ``--disable-threads``, ``--disable-tls`` and ``--disable-libatomic``; a
``__thread`` variable is emulated by libgcc (``__emutls_get_address()``), one
per process.

Atomic operations need no threads. gcc is configured with ``--with-llsc``, and
makes the ``__sync`` and ``__atomic`` operations on objects of 1, 2 and 4
bytes in place as ``ll``/``sc`` sequences, with ``sync`` where the memory order
asks for it, which the core runs (M, the same section; ADR 0021); the build
system claims
them (``BR2_TOOLCHAIN_HAS_SYNC_1``, ``_2`` and ``_4``). An operation on 8 bytes
needs libatomic, which is not built, and does not link: ``undefined reference
to '__atomic_fetch_add_8'``.

The C library's configuration
-----------------------------

``board/dfp-34x-2c3/uclibc.fragment`` is merged over Buildroot's own uClibc-ng
defaults. The architecture, the ABI, the endianness, soft-float, the shared
libraries, the absence of threads and the removal of the MIPS prefetch routines
are set by the toolchain options and the patch, and the fragment does not repeat
them. It names what the userspace of the board needs the C library to offer: the
C99 maths functions, ``ftw``, IPv6 in the library, and ``fopencookie``. The
stack protector is on (``BR2_TOOLCHAIN_BUILDROOT_USE_SSP``), which builds the C
library itself with it as well.

The board's image
=================

``configs/odi_dfp_34x_2c3_defconfig`` builds the kernel and the root filesystem
of the stick and leaves the two flash images in ``images/`` of the output
directory. ``make`` in the top directory does it; ``docs/handbook/building.rst``
has the commands, the sizes and how to put the images on the board. In short:

* The toolchain is the one above: that of the release ``toolchain/PIN``
  names (see "The toolchain of a release"), or, with ``TOOLCHAIN=source`` or a
  pin that names none, the one the board's defconfig builds first. The
  programs are built with shared libraries only (``BR2_SHARED_LIBS``): they
  share one C library, and are not position-independent (``BR2_PIC_PIE`` off,
  so RELRO is partial). The post-build script cuts the shared libraries to
  what the programs use with ``tools/lib-trim``, which removes ``libgcc_s``
  (nothing loads it), links the C library again with only the functions the
  programs import, and fails the build when a program would miss a library or
  a symbol (ADR 0008).
* The kernel is the tree of ``linux/``, taken from the archive that serves the
  headers, configured by ``rtl9602c_defconfig`` and built with this toolchain.
  The image is the LZMA-compressed legacy U-Boot image the boot loader takes.
* The root filesystem is BusyBox 1.37.0 (``board/dfp-34x-2c3/busybox.config``),
  dropbear 2026.93, and the files of ``board/dfp-34x-2c3/overlay``. The build
  system installs no init scripts (``BR2_INIT_NONE``): the overlay's
  ``sbin/init`` starts procd, which reads the overlay's ``etc/inittab`` and runs
  its boot script, ``etc/init.d/rcS`` (ADR 0010). The files that belong to one
  bench and not to the repository, an ssh key or other management addresses, are
  in a directory named by ``OPEN_DFP_34X_2C_LOCAL_OVERLAY``; the post-image
  script lays it over a copy of the target tree, so the target directory keeps
  none of them.
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

What the build judges is in the top-level ``Makefile``: the images against
their partitions (``tools/image-budget``), the instruction-set gate over the
whole target tree, and the digests. ``tests/image/check-applets`` checks that
every command the shell scripts of the image run is in the image, and runs the
image's programs under ``qemu-mips``, with the image's own loader and cut C
library, and ``tests/image/tree-digest`` lists a target tree for the
comparison of two builds; the ``image`` workflow does both
(``.github/workflows/README.rst``).

The toolchain of a release
==========================

The images are built with a toolchain that the repository releases on its
own, the one that ``toolchain/PIN`` names (ADR 0020): a build of the images
builds no compiler, and a commit names its toolchain exactly.

The release
-----------

``.github/workflows/toolchain.yml``, started by hand, builds the toolchain of
``configs/toolchain_defconfig``, checks it (see "Checks on the toolchain"),
with "compare" on builds it again and compares the two builds, and makes a
release of the first, tagged
``toolchain-<version>``, the version being the time of the commit in UTC, as
``2026.10.20-0130``. The release is not marked as the repository's latest,
which stays the firmware's. It holds three files:

``open-dfp-34x-2c-toolchain.tar.gz``
    The relocatable tarball that ``make sdk`` packs: ``host/`` of the build as
    the one directory ``open-dfp-34x-2c-toolchain``, with the pieces of the C
    library's link in ``share/open-dfp-34x-2c/uclibc``; 75668880 bytes on
    2026-10-20 (M).
``open-dfp-34x-2c-toolchain-<version>-sources.tar``
    The sources and licences of everything in the toolchain, gcc, binutils,
    uClibc-ng and the rest: the ``legal-info`` directory of its build;
    290426880 bytes on 2026-10-20 (M).
``SHA256SUMS``
    The SHA-256 digests of the two.

The pieces of the C library's link are what ``tools/lib-trim`` links the
image's C library again from: uClibc-ng's configuration, its archive of
position-independent objects ``libc/libc_so.a``, and its ``lib/``, with the
loader, ``interp.os`` and the libraries of the link. A build that makes its
own toolchain has them in uClibc-ng's build directory. A build with the
toolchain of a release has no such directory, so ``toolchain/keep-uclibc``
copies them into the toolchain before ``make sdk`` packs it, and the
post-build script takes them from there. They lie beside the sysroot, not in
it, because Buildroot copies parts of an external toolchain's sysroot into
the target tree.

The pin
-------

``toolchain/PIN`` holds a ``KEY=VALUE`` line for each of its keys, and every
key has to be there, empty or not; a line that starts with ``#`` is a
comment.

``TAG``
    The tag of the release. Empty, the pin names no release, and the image's
    build makes its own toolchain from source.
``TARBALL``, ``TARBALL_SIZE``, ``TARBALL_SHA256``
    The name of the toolchain's tarball in the release, its size in bytes and
    its SHA-256.
``SOURCES``, ``SOURCES_SIZE``, ``SOURCES_SHA256``
    The same for the archive of its sources and licences.

The pin does not name the repository. ``toolchain/fetch`` takes the release
from the GitHub repository that ``TOOLCHAIN_REPOSITORY`` names, as
``owner/name``, else from ``GITHUB_REPOSITORY``, which a workflow sets, else
from the one of the checkout's remote ``origin``.

Fetching it
-----------

``toolchain/fetch`` reads the pin. A pin without a tag names no release: the
script prints nothing and exits 0. Otherwise it looks for the tarball in the
download directory (``--dl``, else ``$BR2_DL_DIR``, else ``build/dl``) and
fetches it from the release when it is not there. With ``GH_TOKEN`` or
``GITHUB_TOKEN`` set, it fetches the file through GitHub's API with that
token, which reaches a private repository too, and does not hand the token on
when GitHub sends the download to another host; without a token, from the
release's public download address.

It unpacks the tarball into the output directory (``--out``, else
``build/toolchain-release``) as ``<tag>/open-dfp-34x-2c-toolchain``, in a
scratch directory that is swapped in at the end, runs the relocation script
there, and writes a stamp, ``<tag>.stamp``, that holds the tarball's SHA-256.
The path of the toolchain is the only thing it prints on standard output. A
later run that finds the stamp of the pinned tarball does nothing, and needs
neither the tarball nor the network; another pinned tarball is unpacked
again. With ``--sources`` it checks the archive of the sources into the
download directory instead, and prints its path.

It refuses, with exit status 2, a pin that it cannot use: a line that is not
one of its keys, a key that is missing, and, with a tag, a file name that is
not a plain name or that leaves the download directory, a size that is not a
number, or a digest that is not 64 hexadecimal digits. With exit status 1 it
refuses a file it cannot fetch, a file whose size or SHA-256 is not the
pinned one, and a tarball that does not hold one directory with one
relocation script, ``relocate-*.sh``. A download is checked before it gets
its final name, so a file that was refused never stays in the download
directory under the name a later run would trust.
``tests/toolchain/test_fetch.py`` tests the script, without the network.

The image's build
-----------------

``make`` in the top directory builds the images with the toolchain of the
release (``TOOLCHAIN=release``, the default). It runs ``toolchain/fetch``
with the build's download directory and ``TOOLCHAIN_RELEASE_OUT`` (default
``build/toolchain-release``), configures the board's defconfig, merges
``configs/external-toolchain.config`` over it with Buildroot's
``support/kconfig/merge_config.sh`` and runs ``olddefconfig``. The fragment
makes the toolchain Buildroot's external toolchain, a custom one that is
installed already, at the path that ``fetch`` printed
(``OPEN_DFP_34X_2C_TOOLCHAIN``), and says what the toolchain is, which
Buildroot checks against what it finds: the prefix
``mips-buildroot-linux-uclibc``, gcc 14, the kernel headers of 6.12 or later,
uClibc-ng with wide characters and without locales, no threads, the stack
protector, no RPC and no C++. That is the toolchain the board's defconfig
would build. ``external.mk`` stops a build with the fragment that does not set
``OPEN_DFP_34X_2C_TOOLCHAIN``.

The toolchain's sysroot holds the kernel headers of the commit the toolchain
was built from, against which its C library was built. The programs of this
tree are built against the uapi headers of the commit they are built at,
``include/uapi/linux/pon.h`` among them, as with a toolchain built from
source. So ``external.mk`` gives Buildroot's package of the external
toolchain a hook (``TOOLCHAIN_EXTERNAL_CUSTOM_POST_INSTALL_STAGING_HOOKS``):
once the toolchain's sysroot is in the staging directory, the hook unpacks
the kernel archive that the kernel is built from and installs its uapi
headers over the toolchain's with ``make headers_install``, as Buildroot's
``linux-headers`` installs them.

An output directory is built with one toolchain: ``.toolchain`` in it records
``source`` or the path of the release's toolchain, and a build with another
stops; ``make clean``, or another ``IMAGE_OUT``, starts afresh.
``make TOOLCHAIN=source``, and ``make`` with a pin that names no release,
make the toolchain from source first, as the board's defconfig says.
``make legal-info`` adds the release's archive of sources and licences to the
image's, in ``legal-info/toolchain`` (``docs/handbook/building.rst``).

A new toolchain
---------------

A change to what the toolchain is built from -- the pinned Buildroot and its
patches, ``configs/toolchain_defconfig`` or
``board/dfp-34x-2c3/uclibc.fragment`` -- reaches the images through a release
and a commit of the pin. Until then the images keep the pinned toolchain, and
``make TOOLCHAIN=source`` builds an image with the toolchain of the
checked-out tree, for that work. A change to ``linux/`` needs no release: the
images take the uapi headers of their own commit. To release a toolchain and
pin it:

#. Start the workflow ``toolchain`` by hand on the branch that holds the
   commit, from the workflow's page or with
   ``gh workflow run toolchain.yml --ref <branch>``, with "release" on, its
   default. Once the toolchain is built, checked and compared, the job
   ``release`` makes the release ``toolchain-<version>``; it is refused when
   the tag exists already, as a version is released once.
#. Copy the lines of ``toolchain/PIN`` that the release's notes print into
   the file, in place of the lines from ``TAG=`` on.
#. Commit the pin, as a commit of ``br2-external``. The images of that commit
   and of the ones after it are built with the new toolchain.

Checks on the toolchain
=======================

The checks run on the toolchain in ``build/toolchain/host`` or on an unpacked
and relocated tarball. They need ``binutils-mips-linux-gnu``, for the
``objdump`` of the instruction-set gate, ``qemu-user`` and
``gcc-mips-linux-gnu``.

The instruction-set gate over the toolchain's own objects
----------------------------------------------------------

``tools/isa-gate`` is run over every object the toolchain holds for the target:
the start files, the C library as an archive and as a shared library, the
dynamic loader, libgcc in both forms and the helper programs of the C library.
``tests/toolchain/target-objects`` lists them by reading the ELF headers, which
leaves out the programs that run on the build machine::

    br2-external/tests/toolchain/target-objects build/toolchain/host \
        | xargs tools/isa-gate/isa-gate --archives

For the toolchain this defconfig builds, the list holds 30 files, 22 objects
and 8 archives, and the gate scans 5469 objects, 731149 instructions (M,
2026-10-20). It reports 27 findings, all in one function of libgcc:

``__speculation_barrier`` (9 findings in libgcc.a and in each libgcc_s.so.1)
    Eight ``ssnop`` and a ``jr.hb`` word, after a ``sync``, which the gate
    allows; ``libgcc_s.so.1`` is in the toolchain twice. gcc 14 builds it
    for every MIPS target (``LIB1ASMFUNCS`` in
    ``libgcc/config/mips/t-mips``). A program reaches it through
    ``__builtin_speculation_safe_value`` only.

The ``ll``/``sc`` sequences of the C library, in ``__cxa_finalize``, which
claims an entry of the exit-handler table with a compare-and-exchange, and in
``_test_and_set``, the function behind ``<sys/tas.h>``, are not findings: the
core runs them, and the gate allows ``ll``, ``sc`` and ``sync`` (ADR 0021).

No option of gcc leaves the function out. A program that links libgcc
statically takes only the objects it refers to, so a static program does not
contain it; the test programs below, static and dynamic, pass the gate. The
image does not ship ``libgcc_s``, which nothing loads (ADR 0008), and the gate
over the target tree holds the image to that.

``tests/toolchain/known-findings.txt`` lists the findings, for each file,
archive member and function, with the instructions in the order the gate reports
them. ``compare-findings`` holds the gate's output to the list: the toolchain
job passes when the findings are exactly the list, and fails when one is new,
gone or different. On the toolchain this defconfig builds the 27 findings are
the 3 groups of the list. When the toolchain changes on purpose,
``compare-findings --print-known`` writes the new list from the gate's output.

The test programs
-----------------

``tests/toolchain/t-*.c`` are 15 small programs: formatted input and output, the
allocator, the string and memory functions, ``qsort``, calendar and clock
functions, ``poll`` and ``select``, sockets that need no network, signals,
64-bit division, shifts by a run-time count, unaligned loads and stores through
``memcpy``, exit handlers, atomic operations of 1, 2 and 4 bytes (one of them
on a count that a timer's signal handler adds to as well), soft-float
arithmetic and the maths library, and integer division by zero (which the
compiler checks with ``break``). Each checks
its own results and prints ``ok <name>``; the values that some print are checked
against exact arithmetic done outside the programs. ``expected.txt`` holds what
each prints. ::

    br2-external/tests/toolchain/run build/toolchain/host

builds every program with ``-O2`` and ``-Wall -Wextra -Werror``, static and
dynamic, into ``build/toolchain-tests``; runs the gate over the 30 binaries;
runs each of 14 of the programs under ``qemu-mips`` (the dynamic ones with the
toolchain's sysroot) and compares its output with ``expected.txt``; and, for
``t-atomic``, requires the instructions that ``qemu-mips`` translated to hold
``ll`` and ``sc``, the sign that the atomic operations were made in place and
not as calls. On the board, ``t-atomic`` also puts the sequences through the
interrupts and signals that end their reservations, which the emulator does
not do. ``t-divzero`` is built and checked by the gate, and not run under
``qemu-mips``, which raises SIGTRAP for the ``break`` where the kernel of the
board raises SIGFPE.

To run them on the board, copy the ``static`` directory, and ``check-output``
and ``expected.txt`` of ``br2-external/tests/toolchain``, to it, and run::

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

``tests/toolchain/negative-control`` compiles five functions with the Debian
cross compiler for MIPS, whose default is MIPS32 release 2, and requires the
gate to refuse the object: for its architecture flags, for ``teq``, the trap
of MIPS II that this compiler puts after a division, and for the instructions
that a listing as MIPS II leaves as data words (``clz``, ``mul`` and
``wsbh``). It shows that the gate can fail on code that this core cannot run.
The object also holds the ``ll``, ``sc`` and ``sync`` of an atomic addition,
which the core runs, and the gate must not report them.

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
file, and the 16-byte ``executable_checksum`` that the compiler proper (``cc1``)
records for its precompiled-header and plug-in checks. The trees have to hold
the same files, links and modes, and every file has to be identical after that::

    br2-external/tests/toolchain/compare-trees build/toolchain-a/host \
        build/toolchain-b/host toolchain-a toolchain-b

It prints how many files were identical as they were, how many after the
replacement and the setting aside, and the files that still differ; its exit
status is non-zero if there is one. The tarballs that ``make sdk`` writes are
not compared: a tarball holds the modification times of the files as they were
installed.

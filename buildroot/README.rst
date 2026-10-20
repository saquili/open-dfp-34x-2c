.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

=========
buildroot
=========

The pin for the distribution build system (see ADR 0005).

Buildroot itself is not committed here. This directory pins one Buildroot
release by hash and holds the script that turns the pin into a tree ready to
build, so that every build starts from the same bytes. The tree is made in
``build/buildroot`` and is never committed. The patches we carry against the
release are in ``patches/``: three, which add the MIPS I variant that this core
needs, take the licence text of the kernel this distribution builds, and take
the libraries of netifd at newer commits, with json-c built without
thread-local storage.

What is here
============

``VERSION``
    The pin: name, version, URL, size and SHA-256 of the release tarball, and
    the fingerprint of the key that signs it. The release is Buildroot
    2025.02.18, a point release of its long-term-support series 2025.02. Moving
    to another release means editing this file, and the patch if it does not
    apply to the new one.
``fetch``
    The script that makes the pinned release ready to build (below).
``patches/``
    The patches applied to the release, in name order: the MIPS I variant,
    the kernel's licence text and the libraries of netifd (below).
``tests/``
    The self-tests of ``fetch``.

Running it
==========

::

    make buildroot

runs ``buildroot/fetch``, which does the following:

#. looks for the tarball named in the pin's URL in the download directory
   (``$BUILDROOT_DL``, else ``build/dl``) and downloads it from the URL when it
   is not there;
#. refuses the tarball -- exit status 1, nothing extracted -- unless its size
   and SHA-256 are the pinned ones. A download is checked before it gets its
   final name, so a tarball that was refused never stays in the directory under
   the name a later run would trust;
#. unpacks it into ``build/buildroot/buildroot-<version>/``, in a scratch
   directory that is swapped in only when everything has worked;
#. applies ``patches/*.patch`` in name order with ``patch -p1``, without fuzz.
   A patch that does not apply stops the run, is named in the message and
   leaves no tree behind; and
#. writes a stamp, ``build/buildroot/buildroot-<version>.stamp``, that holds the
   pinned SHA-256 and the SHA-256 of each patch.

A run that finds a tree and a stamp equal to what the pin and the patches now
say changes nothing and does not read the tarball or use the network. A changed
pin, or a patch that is changed, added or removed, makes the next run redo the
work from the tarball. The only thing printed on standard output is the path of
the tree. ``make buildroot ARGS=-q`` prints errors only, and
``buildroot/fetch --help`` lists the options that name another pin, patch
directory, download directory or build directory; ``BUILDROOT_DL=<directory>``
names the directory that holds or receives the tarball.

It needs Python 3.11 or later (standard library only) and ``patch``. On
Debian::

    apt-get install python3 patch

The MIPS I patch
================

Buildroot's MIPS menu offers MIPS32 and later, MIPS IV and MIPS64; a MIPS I
entry is not there (``grep BR2_mips_ arch/Config.in.mips`` in the unpacked
release lists the variants). The core of the RTL9602C, a Lexra LX5281, is a big-endian
MIPS I design with no floating-point unit. Of the later instructions it runs
``ll``, ``sc`` and ``sync``, and it traps on or runs wrongly the others that
were tried; it has no thread pointer (M,
``linux/Documentation/arch/mips/rtl9602c.rst``, "Instructions beyond MIPS I";
``docs/adr/0021-atomic-operations-use-the-cores-ll-and-sc.rst``). Every entry
that is offered makes the compiler and the C library produce code for a later
architecture.

``patches/0001-arch-mips-add-a-MIPS-I-variant.patch`` adds the entry, in 9 hunks
of 6 files: 34 insertions and 1 deletion (``git apply --stat``). Its header says
what each hunk is for; in short:

* ``BR2_mips_1`` is the variant, for the big-endian architecture only. It sets
  the compiler's architecture to ``mips1``, so that gcc is configured with
  ``--with-arch=mips1`` and everything it builds, libgcc and the C library
  included, is MIPS I with no flag on a command line. Soft-float is not an
  option for it.
* gcc is also configured with ``--with-divide=breaks`` (a division by zero is
  checked with ``break``; the trap instruction ``teq`` is MIPS II, and the core
  traps on it), ``--with-llsc`` (atomic operations as ``ll``/``sc`` sequences,
  which the core runs) and ``--without-synci``, so the compiler's defaults
  agree with the core.
* The atomic builtins are claimed as the build system claims them for MIPS
  (``BR2_TOOLCHAIN_HAS_SYNC_1``, ``_2`` and ``_4``), so the packages that
  depend on them are offered. The thread libraries of uClibc-ng are not:
  NPTL reaches its storage through the thread pointer, which the core does not
  have, and linuxthreads is not kept up. Without threads the build system
  builds no libatomic, and there is no ``BR2_TOOLCHAIN_HAS_ATOMIC``.
* glibc and musl are not offered; uClibc-ng is the C library of this core. Its
  MIPS prefetch routines, which use a MIPS IV instruction, are switched off.

C only and no multilib are not part of the patch: Buildroot's defaults already
build the C compiler alone, with no multilib, and ``BR2_MIPS_SOFT_FLOAT``
already defaults to on.

uClibc-ng carries no patch of this tree: its atomic operations, such as the
compare-and-exchange with which ``__cxa_finalize()`` claims an entry of the
exit-handler table, are ``ll``/``sc`` sequences that the core runs.

The kernel's licence text
=========================

``patches/0003-linux-take-the-GPL-2.0-text-of-Linux-6.18.patch`` changes one
line of ``linux/linux.hash``, to which
``package/linux-headers/linux-headers.hash`` links. The build system checks the licence files of the kernel and of its
headers against those hashes whatever the kernel's version, and ``make
legal-info`` stops at a file whose hash differs. The hashes are of the GPL-2.0
text of the kernels of this release; Linux 6.18 replaced the Free Software
Foundation's postal address in ``LICENSES/preferred/GPL-2.0`` with its web
address, so the patch gives that file the hash of 6.18's text.

The libraries of netifd
=======================

``patches/0004-*.patch``, whose subject names libubox, ubus and json-c, changes
five files of the three recipes of the libraries of netifd (``netifd/``). The
build system offers all three to the MIPS I variant as it is, since the variant
claims the atomic builtins that json-c and ubus depend on.

* libubox and ubus are taken at OpenWrt's commits of 2026-10-08 and 2026-09-25
  (``da2d205``, ``414c60a``) instead of the release's (``3868f47``,
  ``afa57cc``): procd needs the newer event loop, and netifd and procd are
  taken at their current commits too. The archives' hashes are computed locally. libubox is
  built as OpenWrt builds it, with the client side of udebug, a ring buffer
  that processes share and update with atomic operations, which the compiler
  makes in place.
* json-c keeps one value per thread, the format of the numbers it writes. The
  variant has no threads and emulates thread-local storage with
  ``__emutls_get_address()`` of libgcc_s, which json-c would be the only
  library to load, and so keep in the image. ``package/json-c/json-c.mk``
  builds json-c without thread-local storage on a toolchain without threads
  (``BR2_TOOLCHAIN_HAS_THREADS`` unset); the value is then the process's.
  json-c sets the seed of its hash tables once with a compare-and-swap, which
  the compiler makes in place too.

The instruction-set gate checks what the libraries became, as it checks every
file of the image.

Checking the pin
================

The size and the SHA-256 in ``VERSION`` are checked on every fetch. The
release manager signs a message that lists the same SHA-256; to check that the
pin is the published release, import the key whose fingerprint is in
``VERSION`` and verify the signature::

    gpg --verify buildroot-2025.02.18.tar.xz.sign
    grep SHA256 buildroot-2025.02.18.tar.xz.sign

Tests
=====

::

    buildroot/tests/run [-v]

Each test builds a small release tarball, a pin for it and some patches in a
scratch directory and runs the script as a program. They show that a wrong
hash or size is refused and nothing is extracted, that a patch that does not
apply is named and leaves nothing, that the patches go on in name order, that a
second run is a no-op that needs neither tarball nor network, that a changed,
added or removed patch or a changed pin redoes the work, that a path that
leaves the tree is refused, and that a malformed pin is an error. None uses
the network. They run in ``make check`` and in the ``buildroot-fetch`` job of
``.github/workflows/gates.yml``, which then runs ``make buildroot`` on the real
release: it fetches the release from its URL, checks it against the pin and
applies the carried patches, so that a pin or a patch that changed without the
other fails there.

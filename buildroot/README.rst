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
release are in ``patches/``: two, which add the MIPS I variant that this core
needs and keep the shared C library from an atomic operation the core cannot
run.

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
    The patches applied to the release, in name order: the MIPS I variant
    and the C library's exit handlers (below).
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

Buildroot's MIPS menu offers MIPS32 and later, MIPS IV and MIPS64; a MIPS I entry
is not there (``grep BR2_mips_ arch/Config.in.mips`` in the unpacked release
lists the variants). The core of the RTL9602C, a Lexra LX5281, is a big-endian
MIPS I design: it runs no MIPS32 instruction, has no floating-point unit, and
although it executes the load-linked instruction it never completes a
store-conditional, so an atomic sequence retries for ever
(``tools/isa-gate/README.rst``). Every entry that is offered makes the compiler
and the C library produce code for a later architecture.

``patches/0001-arch-mips-add-a-MIPS-I-variant.patch`` adds the entry, in 11 hunks
of 7 files: 35 insertions and 1 deletion (``git apply --stat``). Its header says
what each hunk is for; in short:

* ``BR2_mips_1`` is the variant, for the big-endian architecture only. It sets
  the compiler's architecture to ``mips1``, so that gcc is configured with
  ``--with-arch=mips1`` and everything it builds, libgcc and the C library
  included, is MIPS I with no flag on a command line. Soft-float is not an
  option for it.
* gcc is also configured with ``--with-divide=breaks`` (a division by zero is
  checked with ``break``; the trap instruction is MIPS II), ``--without-llsc``
  and ``--without-synci``, so the compiler's defaults agree with the core.
* The atomic builtins are not claimed (``BR2_TOOLCHAIN_HAS_SYNC_1``, ``_2`` and
  ``_4`` stay unset), so the packages that depend on them are not offered. The
  thread libraries of uClibc-ng are not offered either, since both rely on
  atomic operations; without threads there is no ``BR2_TOOLCHAIN_HAS_ATOMIC``.
* glibc and musl are not offered; uClibc-ng is the C library of this core. Its
  MIPS prefetch routines, which use a MIPS IV instruction, are switched off.

C only and no multilib are not part of the patch: Buildroot's defaults already
build the C compiler alone, with no multilib, and ``BR2_MIPS_SOFT_FLOAT``
already defaults to on.

The exit-handler patch
======================

``patches/0002-package-uclibc-claim-__cxa_atexit-entries-without-atomics.patch``
adds a patch of uClibc-ng to the build system's own patches of the C library,
``package/uclibc/0005-stdlib-claim-__cxa_atexit-entries-without-atomics.patch``,
which the build system applies whenever it builds uClibc-ng.

``__cxa_finalize()`` marks an entry of the exit-handler table as run with a
compare-and-exchange: a load-linked and store-conditional pair, which on this
core retries for ever. Every shared C library holds the function, and the
start files of each shared object call it when the object is unloaded, so a
program linked against the shared C library can reach it at exit. Without
threads, the only configuration that the MIPS I variant offers, the patch
claims the entry with a plain test and store; in a program with one thread
nothing comes between the two. With threads the function is as it was.

The C library's other function of this kind, ``_test_and_set()`` of
``<sys/tas.h>``, is left alone. It promises atomicity to processes that share
memory, which a plain test and store does not keep, and no program of the image
calls it; the cut of the shared C library to what the programs use leaves it out
(``tools/lib-trim/README.rst``), and the instruction-set gate would refuse an
image that brought it back.

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

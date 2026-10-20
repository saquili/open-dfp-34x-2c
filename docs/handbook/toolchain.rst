.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================
The cross toolchain
===================

How to build a program for the stick: the compiler the distribution carries,
what a program for this core must not do, and how to check one. How the
toolchain is built, and the checks that it passes, are in
``br2-external/README.rst``; the core is described in ``board.rst`` and in the
kernel's ``rtl9602c.rst``.

What it is
==========

A cross compiler for the core, built from source by the distribution's build
system: gcc 14.4.0 (C only), binutils 2.44 and uClibc-ng 1.0.59, with the
kernel headers of this repository's own kernel. Its target is
``mips-buildroot-linux-uclibc``: big-endian MIPS I, 32-bit, soft-float. It
comes as a relocatable tarball that is unpacked and used anywhere.

Where it comes from
===================

The repository releases the toolchain on its own, and the images are built
with the release that ``br2-external/toolchain/PIN`` names
(``docs/adr/0020-images-are-built-with-a-pinned-toolchain-release.rst``). A
release is tagged ``toolchain-<version>``, the version being the time of its
commit in UTC, as ``2026.10.20-0130``, and holds three files:

``open-dfp-34x-2c-toolchain.tar.gz``
    The toolchain, with one directory, ``open-dfp-34x-2c-toolchain``.
``open-dfp-34x-2c-toolchain-<version>-sources.tar``
    The sources and licences of everything in it.
``SHA256SUMS``
    The SHA-256 digests of the two.

While the pin names a release, ``br2-external/toolchain/fetch`` gives the
toolchain the images are built with: it fetches the tarball, checks it
against the pin, unpacks and relocates it in ``build/toolchain-release``, and
prints its directory, whose ``bin`` holds the compiler::

    PATH=$(br2-external/toolchain/fetch)/bin:$PATH

A tarball taken by hand from the release is checked against ``SHA256SUMS``
and the pin, unpacked anywhere, and made to work there by the script
``./relocate-*.sh`` in its directory. ``br2-external/README.rst``, "The
toolchain of a release", says what the release holds and how a new one is
made and pinned.

``make sdk`` builds the same toolchain from the checked-out tree instead, and
leaves the tarball in ``build/toolchain/images``; ``building.rst`` gives the
time it takes, and ``br2-external/README.rst`` the steps. After ``make``,
``build/image/host/bin`` holds the compiler of the image's build, whichever
toolchain it took.

Compiling
=========

The compiler's defaults are those of the core, so no option names the
architecture::

    t=mips-buildroot-linux-uclibc
    $t-gcc -O2 -o hello hello.c            # dynamic
    $t-gcc -O2 -static -o hello hello.c    # static

The code it makes is not position-independent, as the image's programs are
not (``BR2_PIC_PIE`` is off in ``toolchain_defconfig``).

``$t-gcc -v`` shows ``--with-arch=mips1``, ``--with-float=soft``,
``--with-divide=breaks`` and ``--with-llsc`` in its configuration, and an object
made with no options has the ELF flags ``mips1`` and ``o32`` and the ABI flag
``FP ABI: Soft float`` (``$t-readelf -h`` and ``-A``).

Atomic operations
=================

The core runs ``ll``, ``sc`` and ``sync`` of MIPS II, in user and in kernel
mode, and the instruction after an ``ll`` sees its result, as after an ``lw``
(M, measured on the DFP-34X-2C3 on 2026-10-20; the kernel's ``rtl9602c.rst``,
"Instructions beyond MIPS I", has the probes and the counts). The compiler makes
the ``__sync`` and the C11 atomic operations on objects of 1, 2 and 4 bytes in
place as ``ll``/``sc`` sequences, with ``sync`` where the memory order asks for
it, and the instruction-set gate allows the three instructions
(``docs/adr/0021-atomic-operations-use-the-cores-ll-and-sc.rst``). An ``sc``
fails when an exception comes between it and its ``ll``, an interrupt or the
page fault of the first store to a page not yet written among them (M), so
every sequence starts again until its ``sc`` succeeds; those of the compiler do.

What a program for this core must not do
========================================

**Use threads.** The core has no thread pointer: ``rdhwr $3, $29``, with which
the MIPS ABI reads it, faults as an access to memory instead of trapping, so the
kernel cannot emulate it (M). The toolchain has no thread library, and
``<pthread.h>`` is not installed. A ``__thread`` variable compiles, and libgcc
emulates it with one copy per process (``__emutls_get_address()``); a dynamic
program that has one needs ``libgcc_s``, which the image does not ship.

**Use an atomic operation on 8 bytes.** It needs libatomic, which a toolchain
without threads does not build: ``__sync_fetch_and_add`` on a ``long long``
fails to link, with ``undefined reference to '__sync_fetch_and_add_8'``, and
C11 atomics the same way, on ``__atomic_fetch_add_8``.

**Count on one try of an ll/sc sequence.** A sequence written by hand has to
start again when its ``sc`` fails, as above; one that tries once fails on every
first store to a page not yet written.

**Use other instructions of MIPS II or later.** The core traps on ``mul``,
``clz``, ``teq``, ``beql`` and ``bnel``, and runs ``bltzl`` wrongly, without a
signal (M). The compiler's defaults make none of them; a program that is given
``-march=mips2`` or later does get them, which is what the instruction-set gate
below is for.

**Count on fast floating point.** There is no floating-point unit: ``float`` and
``double`` work, through the compiler's library, and are slow. A 64-bit division
is a call to that library, and a 32-bit division is the ``div`` instruction. A
division by zero is caught with a ``break`` instruction, which the kernel turns
into ``SIGFPE``.

Unaligned loads and stores, as they come out of ``memcpy`` of a few bytes or of
a packed structure, use ``lwl``, ``lwr``, ``swl`` and ``swr``, which the core
runs at all four byte offsets.

Static or dynamic
=================

Programs of both forms pass the instruction-set gate. They differ in what they
need on the board. A static program holds the C library objects it refers to
and needs nothing else. A dynamic program needs the dynamic loader and the C
library there. The image of the board has both: its programs share one C
library, which the build cuts to the functions those programs import
(``tools/lib-trim``, ``building.rst``,
``docs/adr/0008-target-executables-share-one-cut-c-library.rst``). A dynamic
program built apart from the image runs on the board only if that library holds
every function it imports; otherwise the loader names the first one it misses.
For a program copied onto a running board, link statically.

The toolchain's own libraries are whole. ``libgcc_s`` carries a function that
the gate refuses, the speculation barrier of libgcc (``br2-external/README.rst``
says when a program reaches it); the image leaves ``libgcc_s`` out, and the gate
runs over its whole target tree.

Checking a program
==================

The instruction-set gate reads every instruction of the binaries it is given and
refuses what the core does not run::

    tools/isa-gate/isa-gate hello

It needs the cross binutils' ``objdump``; ``tools/isa-gate/README.rst``
describes what it checks and its exit status.

Running the test programs on the board
======================================

``br2-external/tests/toolchain`` holds 15 programs that exercise the C library,
the compiler's runtime and the kernel's system calls, each of which checks its
own results. ``qemu-mips`` runs all but one of them on a build machine. The
board runs them as they are, and shows what the emulator does not: the kernel's
``SIGFPE`` for a division by zero (``t-divzero``), and atomic sequences that
interrupts and signals make start again (``t-atomic``).
Copy the ``static`` directory that ``run`` builds, with ``check-output`` and
``expected.txt``, to the board and run ``sh check-output static``;
``br2-external/README.rst`` has the steps for the dynamic programs.

.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

===================
The cross toolchain
===================

How to build a program for the stick: the compiler the distribution carries, what
a program for this core must not do, and how to check one. How the toolchain is
built, and the checks that it passes, are in ``br2-external/README.rst``; the core
is described in ``board.rst`` and in the kernel's ``rtl9602c.rst``.

What it is
==========

A cross compiler for the core, built from source by the distribution's build
system: gcc 14.4.0 (C only), binutils 2.44 and uClibc-ng 1.0.59, with the kernel
headers of this repository's own kernel. Its target is
``mips-buildroot-linux-uclibc``: big-endian MIPS I, 32-bit, soft-float. Building
it takes about four minutes on an 18-core machine, and it can be had as a
relocatable tarball that is unpacked and used anywhere; the commands are in
``br2-external/README.rst``.

Compiling
=========

The compiler's defaults are those of the core, so no option names the
architecture::

    t=mips-buildroot-linux-uclibc
    $t-gcc -O2 -o hello hello.c            # dynamic, a position-independent executable
    $t-gcc -O2 -static -o hello hello.c    # static

``$t-gcc -v`` shows ``--with-arch=mips1``, ``--with-float=soft`` and
``--with-divide=breaks`` in its configuration, and an object made with no
options has the ELF flags ``mips1`` and ``o32`` and the ABI flag ``FP ABI: Soft
float`` (``$t-readelf -h`` and ``-A``).

What a program for this core must not do
========================================

**Use atomic operations or threads.** The core runs the load-linked instruction
but never completes a store-conditional, so an atomic sequence retries for ever
(``tools/isa-gate/README.rst``). The toolchain does not offer what would need
them. ``__sync_fetch_and_add`` fails to link, with ``undefined reference to
'__sync_fetch_and_add_4'``; C11 atomics fail the same way, on
``__atomic_fetch_add_4``; and ``<pthread.h>`` is not installed. A program that is
given ``-march=mips2`` and ``-mllsc`` does get the instructions, and spins, which
is what the instruction-set gate below is for.

**Count on fast floating point.** There is no floating-point unit:
``float`` and ``double`` work, through the compiler's library, and are slow. A
64-bit division is a call to that library, and a 32-bit division is the ``div``
instruction. A division by zero is caught with a ``break`` instruction, which the
kernel turns into ``SIGFPE``.

Unaligned loads and stores, as they come out of ``memcpy`` of a few bytes or of a
packed structure, use ``lwl``, ``lwr``, ``swl`` and ``swr``, which the core
runs at all four byte offsets.

Static or dynamic
=================

Programs of both forms pass the instruction-set gate. They differ in what goes into
the root filesystem. A static program holds only the C library objects it
refers to. A dynamic program needs the dynamic loader and the C library on the
board, and those carry three functions that the gate refuses (a speculation
barrier of libgcc, and an exit-handler compare-and-exchange and a test-and-set of
uClibc-ng; ``br2-external/README.rst`` lists them and says when a program reaches
them). For what goes into the root filesystem, link statically. The image of the
board follows this rule: it is built with static libraries only, and the gate runs
over its whole target tree (``building.rst``,
``docs/adr/0010-target-executables-are-linked-statically.rst``).

Checking a program
==================

The instruction-set gate reads every instruction of the binaries it is given and
refuses what the core does not run::

    tools/isa-gate/isa-gate hello

It needs the cross binutils' ``objdump``; ``tools/isa-gate/README.rst`` describes
what it checks and its exit status.

Running the test programs on the board
======================================

``br2-external/tests/toolchain`` holds 14 programs that exercise the C library,
the compiler's runtime and the kernel's system calls, each of which checks its
own results. ``qemu-mips`` runs all but one of them on a build machine. The board
runs them as they are, and shows what the emulator does not: the kernel's
``SIGFPE`` for a division by zero (``t-divzero``), and any atomic sequence on the
path of a program, which the emulator completes and the board never does. Copy the
``static`` directory that ``run`` builds, with ``check-output`` and
``expected.txt``, to the board and run ``sh check-output static``;
``br2-external/README.rst`` has the steps for the dynamic programs.
